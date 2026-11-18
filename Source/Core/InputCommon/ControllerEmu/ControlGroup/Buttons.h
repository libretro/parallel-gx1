// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

namespace ControllerEmu
{
class Buttons : public ControlGroup
{
public:
  explicit Buttons(const std::string& name_);
  Buttons(std::string ini_name, std::string group_name);

  template <typename C>
  void GetState(C* const buttons, const C* bitmasks) const
  {
    int i;
    for (i = 0; i < num_controls; i++)
      *buttons |= *(bitmasks++) * controls[i]->GetState<bool>();
  }
};
}  // namespace ControllerEmu
