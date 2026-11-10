// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"

#include <map>
#include <string>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/HiresTextures.h"

GraphicsModGroupConfig::GraphicsModGroupConfig(std::string game_id) : m_game_id(std::move(game_id))
{
}

GraphicsModGroupConfig::~GraphicsModGroupConfig() = default;

GraphicsModGroupConfig::GraphicsModGroupConfig(const GraphicsModGroupConfig&) = default;

GraphicsModGroupConfig::GraphicsModGroupConfig(GraphicsModGroupConfig&&) noexcept = default;

GraphicsModGroupConfig& GraphicsModGroupConfig::operator=(const GraphicsModGroupConfig&) = default;

GraphicsModGroupConfig&
GraphicsModGroupConfig::operator=(GraphicsModGroupConfig&&) noexcept = default;

void GraphicsModGroupConfig::Load()
{
  const std::string file_path = GetPath();

  std::set<std::string> known_paths;
  if (File::Exists(file_path))
  {
    auto mods = LoadGraphicsModProfile(file_path);
    if (!mods)
      return;
    for (auto& mod : *mods)
    {
      known_paths.insert(mod.GetAbsolutePath());
      m_graphics_mods.push_back(std::move(mod));
    }
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
        if (known_paths.contains(file_copy))
          return;

        if (auto mod = GraphicsModConfig::Create(file_copy, source))
          m_graphics_mods.push_back(std::move(*mod));
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
  for (size_t i = 1; i < m_graphics_mods.size(); i++)
  {
    for (size_t j = i; j > 0 && m_graphics_mods[j - 1].m_weight > m_graphics_mods[j].m_weight; j--)
      std::swap(m_graphics_mods[j - 1], m_graphics_mods[j]);
  }
  for (auto& mod : m_graphics_mods)
  {
    m_path_to_graphics_mod[mod.GetAbsolutePath()] = &mod;
  }

  m_change_count++;
}

void GraphicsModGroupConfig::SetChangeCount(u32 change_count)
{
  m_change_count = change_count;
}

u32 GraphicsModGroupConfig::GetChangeCount() const
{
  return m_change_count;
}

const std::vector<GraphicsModConfig>& GraphicsModGroupConfig::GetMods() const
{
  return m_graphics_mods;
}

std::vector<GraphicsModConfig>& GraphicsModGroupConfig::GetMods()
{
  return m_graphics_mods;
}

GraphicsModConfig* GraphicsModGroupConfig::GetMod(std::string_view absolute_path) const
{
  if (const auto iter = m_path_to_graphics_mod.find(absolute_path);
      iter != m_path_to_graphics_mod.end())
  {
    return iter->second;
  }

  return nullptr;
}

const std::string& GraphicsModGroupConfig::GetGameID() const
{
  return m_game_id;
}

std::string GraphicsModGroupConfig::GetPath() const
{
  const std::string game_mod_root = File::GetUserPath(D_CONFIG_IDX) + GRAPHICSMOD_CONFIG_DIR;
  return fmt::format("{}/{}.json", game_mod_root, m_game_id);
}
