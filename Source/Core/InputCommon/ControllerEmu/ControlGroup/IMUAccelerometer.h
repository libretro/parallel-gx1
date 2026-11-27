// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

namespace ControllerEmu
{
class IMUAccelerometer : public ControlGroup
{
public:
  using StateData = Common::Vec3;

  IMUAccelerometer(const char* name, const char* ui_name);

  // Returns false, leaving state as it was, when no input is bound.
  bool GetState(StateData* state) const;

  bool AreInputsBound() const;
};
}  // namespace ControllerEmu
