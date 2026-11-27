// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class IMUCursor : public ControlGroup
{
public:
  IMUCursor(const char* name, const char* ui_name);

  // Yaw movement in radians.
  ControlState GetTotalYaw() const;

  ControlState GetAccelWeight() const;

private:
  SettingValue<double> m_yaw_setting;
  SettingValue<double> m_accel_weight_setting;
};
}  // namespace ControllerEmu
