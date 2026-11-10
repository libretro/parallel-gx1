// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace DSP
{
bool DumpDSPCode(const u8* code_be, size_t size_in_bytes, u32 crc);
}  // namespace DSP
