// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"

class GraphicsModGroupConfig
{
public:
  explicit GraphicsModGroupConfig(std::string game_id);
  ~GraphicsModGroupConfig();

  GraphicsModGroupConfig(const GraphicsModGroupConfig&) = delete;
  GraphicsModGroupConfig& operator=(const GraphicsModGroupConfig&) = delete;

  void Load();

  // The mods, by weight
  const GraphicsModConfig* GetMods(u32* count) const;

  const std::string& GetGameID() const;

private:
  std::string GetPath() const;
  std::string m_game_id;
  GraphicsModList m_graphics_mods = {};
};
