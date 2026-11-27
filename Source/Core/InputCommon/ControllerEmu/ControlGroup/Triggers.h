// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class Triggers : public ControlGroup
{
public:
  explicit Triggers(const char* name);

  // Writes count trigger states to out, 0 for any past the group's triggers.
  void GetState(ControlState* out, std::size_t count) const;

private:
  SettingValue<double> m_deadzone_setting;
};
}  // namespace ControllerEmu
