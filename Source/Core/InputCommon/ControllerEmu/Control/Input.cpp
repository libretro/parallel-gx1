// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/Control/Input.h"

#include <string>
#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
Input::Input(Translatability translate_, const char* name_, const char* ui_name_)
    : Control(ControlReference::Kind::Input, translate_, name_, ui_name_)
{
}

Input::Input(Translatability translate_, const char* name_)
    : Control(ControlReference::Kind::Input, translate_, name_)
{
}
}  // namespace ControllerEmu
