// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/DynamicInputTextureManager.h"

#include <set>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"

#include "InputCommon/DynamicInputTextures/DITConfiguration.h"
#include "InputCommon/ImageOperations.h"
#include "VideoCommon/HiresTextures.h"

namespace InputCommon
{
DynamicInputTextureManager::DynamicInputTextureManager() = default;

DynamicInputTextureManager::~DynamicInputTextureManager() = default;

void DynamicInputTextureManager::Load()
{
  m_configuration.clear();
  const std::set<std::string> dynamic_input_directories =
      GetTextureDirectoriesForFirstMatchingGameId(File::GetUserPath(D_DYNAMICINPUT_IDX),
                                                  SConfig::GetInstance().GetGameIDsForTextures());

  for (const auto& dynamic_input_directory : dynamic_input_directories)
  {
    path_list_t json_files;
    path_list_init(&json_files);
    Common::DoFileSearch(dynamic_input_directory.c_str(), ".json", false, &json_files);
    for (unsigned i = 0; i < json_files.count; i++)
      m_configuration.emplace_back(path_list_get(&json_files, i));
    path_list_free(&json_files);
  }
}

void DynamicInputTextureManager::GenerateTextures(const Common::IniFile& file,
                                                  const std::vector<std::string>& controller_names)
{
  DynamicInputTextures::OutputDetails output;
  for (const auto& configuration : m_configuration)
  {
    configuration.GenerateTextures(file, controller_names, &output);
  }
  // write to highest preference gameid for textures
  const std::string& game_id_for_textures = SConfig::GetInstance().GetGameIDForTextures();
  for (const auto& [generated_folder_name, images] : output)
  {
    const auto hi_res_folder = File::GetUserPath(D_HIRESTEXTURES_IDX) + generated_folder_name;

    if (!File::IsDirectory(hi_res_folder))
    {
      File::CreateDir(hi_res_folder);
    }

    const auto game_id_folder = hi_res_folder + DIR_SEP + "gameids";
    if (!File::IsDirectory(game_id_folder))
    {
      File::CreateDir(game_id_folder);
    }

    File::CreateEmptyFile(game_id_folder + DIR_SEP + game_id_for_textures + ".txt");

    for (const auto& [image_name, image] : images)
    {
      WriteImage(hi_res_folder + DIR_SEP + image_name, image);
    }
  }
}
}  // namespace InputCommon
