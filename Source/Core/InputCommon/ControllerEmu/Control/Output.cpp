// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/Control/Output.h"

#include <string>
#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
Output::Output(Translatability translate_, const char* name_)
    : Control(ControlReference::Kind::Output, translate_, name_)
{
}
}  // namespace ControllerEmu
