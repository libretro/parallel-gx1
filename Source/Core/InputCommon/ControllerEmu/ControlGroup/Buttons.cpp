// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

#include <string>
#include <utility>

namespace ControllerEmu
{
Buttons::Buttons(const char* name_) : Buttons(name_, name_)
{
}

Buttons::Buttons(const char* ini_name, const char* group_name)
    : ControlGroup(ini_name, group_name, GroupType::Buttons)
{
}

}  // namespace ControllerEmu
