// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"

#include <string>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/PathList.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/HiresTextures.h"

GraphicsModGroupConfig::GraphicsModGroupConfig(std::string game_id) : m_game_id(std::move(game_id))
{
}

GraphicsModGroupConfig::~GraphicsModGroupConfig()
{
  FreeGraphicsModList(&m_graphics_mods);
}

void GraphicsModGroupConfig::Load()
{
  const std::string file_path = GetPath();

  // The profile's mods, then those found in the folders that it does not list
  path_list_t known_paths;
  path_list_init(&known_paths);
  if (File::Exists(file_path))
  {
    if (!LoadGraphicsModProfile(file_path, &m_graphics_mods))
    {
      FreeGraphicsModList(&m_graphics_mods);
      path_list_free(&known_paths);
      return;
    }
    for (u32 i = 0; i < m_graphics_mods.count; i++)
      path_list_add(&known_paths, m_graphics_mods.mods[i].GetAbsolutePath().c_str());
  }

  const auto try_add_mod = [&known_paths, this](const std::string& dir,
                                                GraphicsModConfig::Source source) {
    const auto files = Common::DoFileSearch(dir, ".json", true);
    for (const auto& file : files)
    {
      std::string basename;
      SplitPath(file, nullptr, &basename, nullptr);
      if (basename == "metadata")
      {
        auto file_copy = file;
        UnifyPathSeparators(file_copy);
        if (path_list_find(&known_paths, file_copy.c_str()) >= 0)
          return;

        GraphicsModConfig* const mod = AddGraphicsMod(&m_graphics_mods);
        if (mod && !GraphicsModConfig::Create(file_copy, source, mod))
          FreeGraphicsModConfig(&m_graphics_mods.mods[--m_graphics_mods.count]);
      }
    }
  };

  const std::set<std::string> graphics_mod_user_directories =
      GetTextureDirectoriesWithGameId(File::GetUserPath(D_GRAPHICSMOD_IDX), m_game_id);

  for (const auto& graphics_mod_directory : graphics_mod_user_directories)
  {
    try_add_mod(graphics_mod_directory, GraphicsModConfig::Source::User);
  }

  const std::set<std::string> graphics_mod_system_directories = GetTextureDirectoriesWithGameId(
      File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR, m_game_id);

  for (const auto& graphics_mod_directory : graphics_mod_system_directories)
  {
    try_add_mod(graphics_mod_directory, GraphicsModConfig::Source::System);
  }

  // by weight; mods of equal weight stay in the order they were found
  GraphicsModConfig* const mods = m_graphics_mods.mods;
  for (u32 i = 1; i < m_graphics_mods.count; i++)
  {
    for (u32 j = i; j > 0 && mods[j - 1].m_weight > mods[j].m_weight; j--)
    {
      const GraphicsModConfig moved = mods[j - 1];
      mods[j - 1] = mods[j];
      mods[j] = moved;
    }
  }
  path_list_free(&known_paths);
}

const GraphicsModConfig* GraphicsModGroupConfig::GetMods(u32* count) const
{
  *count = m_graphics_mods.count;
  return m_graphics_mods.mods;
}

const std::string& GraphicsModGroupConfig::GetGameID() const
{
  return m_game_id;
}

std::string GraphicsModGroupConfig::GetPath() const
{
  const std::string game_mod_root = File::GetUserPath(D_CONFIG_IDX) + GRAPHICSMOD_CONFIG_DIR;
  return game_mod_root + "/" + m_game_id + ".json";
}
