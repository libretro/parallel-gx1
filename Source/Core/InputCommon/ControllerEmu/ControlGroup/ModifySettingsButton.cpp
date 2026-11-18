// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/ModifySettingsButton.h"

#include <string>
#include <utility>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "Core/Core.h"

namespace ControllerEmu
{
ModifySettingsButton::ModifySettingsButton(const std::string& button_name) : Buttons(button_name)
{
}

void ModifySettingsButton::AddInput(std::string button_name, bool toggle)
{
  if (num_controls >= MAX_HOTKEYS)
    return;
  m_hotkeys[num_controls].toggle = toggle;
  ControlGroup::AddInput(Translatability::Translate, std::move(button_name));
}

void ModifySettingsButton::UpdateState()
{
  for (int i = 0; i < num_controls; ++i)
  {
    const bool state = controls[i]->GetState<bool>();
    Hotkey* const hotkey = &m_hotkeys[i];

    if (!hotkey->toggle)
    {
      // not toggled
      hotkey->active = state;
    }
    else
    {
      // toggle (loading savestates does not en-/disable toggle)
      // after we passed the threshold, we en-/disable. but after that, we don't change it
      // anymore
      if (!hotkey->threshold_passed && state)
      {
        hotkey->active = !hotkey->active;

        if (hotkey->active)
          Core::DisplayMessage("%s: on", controls[i]->ui_name.c_str());
        else
          Core::DisplayMessage("%s: off", controls[i]->ui_name.c_str());

        hotkey->threshold_passed = true;
      }

      if (!state)
        hotkey->threshold_passed = false;
    }
  }
}
}  // namespace ControllerEmu
