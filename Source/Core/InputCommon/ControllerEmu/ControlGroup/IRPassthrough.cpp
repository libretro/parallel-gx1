// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/IRPassthrough.h"

#include <algorithm>
#include <string>

#include "Common/Common.h"

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
IRPassthrough::IRPassthrough(const char* name_, const char* ui_name_)
    : ControlGroup(name_, ui_name_, GroupType::IRPassthrough,
                   ControlGroup::DefaultValue::Disabled)
{
  AddInput(Translatability::Translate, _trans("Object 1 X"));
  AddInput(Translatability::Translate, _trans("Object 1 Y"));
  AddInput(Translatability::Translate, _trans("Object 1 Size"));
  AddInput(Translatability::Translate, _trans("Object 2 X"));
  AddInput(Translatability::Translate, _trans("Object 2 Y"));
  AddInput(Translatability::Translate, _trans("Object 2 Size"));
  AddInput(Translatability::Translate, _trans("Object 3 X"));
  AddInput(Translatability::Translate, _trans("Object 3 Y"));
  AddInput(Translatability::Translate, _trans("Object 3 Size"));
  AddInput(Translatability::Translate, _trans("Object 4 X"));
  AddInput(Translatability::Translate, _trans("Object 4 Y"));
  AddInput(Translatability::Translate, _trans("Object 4 Size"));
}

ControlState IRPassthrough::GetObjectPositionX(size_t object_index) const
{
  return controls[object_index * 3 + 0]->GetState();
}

ControlState IRPassthrough::GetObjectPositionY(size_t object_index) const
{
  return controls[object_index * 3 + 1]->GetState();
}

ControlState IRPassthrough::GetObjectSize(size_t object_index) const
{
  return controls[object_index * 3 + 2]->GetState();
}

bool IRPassthrough::AreInputsBound() const
{
  int i;
  for (i = 0; i < num_controls; i++)
  {
    if (controls[i]->control_ref->BoundCount() > 0)
      return true;
  }
  return false;
}

}  // namespace ControllerEmu
