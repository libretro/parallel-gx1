// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace ControllerEmu
{
class ModifySettingsButton : public Buttons
{
public:
  explicit ModifySettingsButton(const std::string& button_name);

  // Room for the hotkeys a controller has
  enum
  {
    MAX_HOTKEYS = 8
  };

  void AddInput(std::string button_name, bool toggle = false);

  void UpdateState();

  // Whether the setting of the hotkey at that index is on
  bool IsSettingActive(int index) const { return m_hotkeys[index].active; }

private:
  struct Hotkey
  {
    bool toggle;            // toggled rather than held
    bool threshold_passed;  // pressed since it last toggled
    bool active;            // the result
  };

  Hotkey m_hotkeys[MAX_HOTKEYS] = {};
};
}  // namespace ControllerEmu
