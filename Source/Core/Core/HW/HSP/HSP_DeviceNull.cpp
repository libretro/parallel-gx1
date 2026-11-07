// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/HSP/HSP_DeviceNull.h"

namespace HSP
{

void CHSPDevice_Null::Read(u32 address, u8* data)
{
}

void CHSPDevice_Null::Write(u32 address, const u8* data)
{
}

}  // namespace HSP
