// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"

#include <algorithm>

#include "Common/Common.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
IMUAccelerometer::IMUAccelerometer(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IMUAccelerometer)
{
  AddInput(Translatability::Translate, _trans("Up"));
  AddInput(Translatability::Translate, _trans("Down"));
  AddInput(Translatability::Translate, _trans("Left"));
  AddInput(Translatability::Translate, _trans("Right"));
  AddInput(Translatability::Translate, _trans("Forward"));
  AddInput(Translatability::Translate, _trans("Backward"));
}

bool IMUAccelerometer::AreInputsBound() const
{
  int i;
  for (i = 0; i < num_controls; i++)
  {
    if (controls[i]->control_ref->BoundCount() > 0)
      return true;
  }
  return false;
}

bool IMUAccelerometer::GetState(StateData* state) const
{
  if (!AreInputsBound())
    return false;

  state->x = (controls[2]->GetState() - controls[3]->GetState());
  state->y = (controls[5]->GetState() - controls[4]->GetState());
  state->z = (controls[0]->GetState() - controls[1]->GetState());
  return true;
}

}  // namespace ControllerEmu
