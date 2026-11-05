// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/ActionReplay.h"
#include "Core/GeckoCode.h"

namespace DiscIO
{
class Volume;
}

class AchievementManager
{
public:
  static AchievementManager& GetInstance()
  {
    static AchievementManager s_instance;
    return s_instance;
  }

  constexpr bool IsHardcoreModeActive() { return false; }

  constexpr bool ShouldGeckoCodeBeActivated(const Gecko::GeckoCode& code, std::string_view game_id,
                                            u16 revision)
  {
    return code.enabled;
  }

  constexpr bool ShouldARCodeBeActivated(const ActionReplay::ARCode& code, std::string_view game_id,
                                         u16 revision)
  {
    return code.enabled;
  }

  constexpr void LoadGame(const DiscIO::Volume*) {}

  constexpr void ChangeDisc(const DiscIO::Volume*) {}

  constexpr void SetBackgroundExecutionAllowed(bool allowed) {}

  constexpr void DoFrame() {}

  constexpr void CloseGame() {}
};

