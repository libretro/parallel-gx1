// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Config
{
// Layers in ascending order of priority.
enum class LayerType
{
  Base,
  CommandLine,
  GlobalGame,
  LocalGame,
  Movie,
  CurrentRun,
  Meta,
};

enum class System
{
  Main,
  SYSCONF,
  GCPad,
  WiiPad,
  GCKeyboard,
  GFX,
  Logger,
  Session,
  GameSettingsOnly,
  Achievements,
};

constexpr LayerType SEARCH_ORDER[7]{
    LayerType::CurrentRun, LayerType::Movie,       LayerType::LocalGame,
    LayerType::GlobalGame, LayerType::CommandLine, LayerType::Base,
};
}  // namespace Config
