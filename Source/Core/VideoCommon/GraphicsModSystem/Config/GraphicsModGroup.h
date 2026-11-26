// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

struct GraphicsModConfig;

class GraphicsModGroupConfig
{
public:
  explicit GraphicsModGroupConfig(std::string game_id);
  ~GraphicsModGroupConfig();

  GraphicsModGroupConfig(const GraphicsModGroupConfig&) = delete;
  GraphicsModGroupConfig& operator=(const GraphicsModGroupConfig&) = delete;

  void Load();

  const std::vector<GraphicsModConfig>& GetMods() const;
  std::vector<GraphicsModConfig>& GetMods();

  const std::string& GetGameID() const;

private:
  std::string GetPath() const;
  std::string m_game_id;
  std::vector<GraphicsModConfig> m_graphics_mods;
};
