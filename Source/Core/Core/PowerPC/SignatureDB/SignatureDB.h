// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace SignatureDB
{
// A checksum of the opcodes from @offsetStart to @offsetEnd (inclusive), leaving out their
// operands, which change between builds of the same function: what HLE finds functions by.
u32 ComputeCodeChecksum(const Core::CPUThreadGuard& guard, u32 offsetStart, u32 offsetEnd);
}  // namespace SignatureDB
