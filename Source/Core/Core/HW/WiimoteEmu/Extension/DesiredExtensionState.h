// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>

#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"

namespace WiimoteEmu
{
// The extension input of one update. "type" names the active member of "data",
// in ExtensionNumber order for DTM file index consistency.
struct DesiredExtensionState
{
  ExtensionNumber type = ExtensionNumber::NONE;
  union
  {
    u8 none;
    Nunchuk::DesiredState nunchuk;
    Classic::DesiredState classic;
    Guitar::DesiredState guitar;
    Drums::DesiredState drums;
    Turntable::DesiredState turntable;
    UDrawTablet::DesiredState udraw_tablet;
    DrawsomeTablet::DesiredState drawsome_tablet;
    TaTaCon::DesiredState tatacon;
    Shinkansen::DesiredState shinkansen;
  } data = {};
};

// Copies "size" bytes of desired input into the controller data,
// or zeroes them when the desired state is for another extension.
inline void DefaultExtensionUpdate(EncryptedExtension::Register* reg, const void* desired,
                                   size_t size)
{
  if (desired)
    memcpy(&reg->controller_data, desired, size);
  else
    memset(&reg->controller_data, 0, size);
}
}  // namespace WiimoteEmu
