// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

#include <cstddef>
#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
Triggers::Triggers(const char* name_) : ControlGroup(name_, GroupType::Triggers)
{
  AddDeadzoneSetting(&m_deadzone_setting, 50);
}

void Triggers::GetState(ControlState* out, std::size_t count) const
{
  const std::size_t trigger_count = static_cast<std::size_t>(num_controls);
  const ControlState deadzone = m_deadzone_setting.GetValue() / 100;

  for (std::size_t i = 0; i < count; ++i)
  {
    ControlState state = 0;
    if (i < trigger_count)
    {
      state = ApplyDeadzone(controls[i]->GetState(), deadzone);
      if (state > 1.0)
        state = 1.0;
    }
    out[i] = state;
  }
}

}  // namespace ControllerEmu
