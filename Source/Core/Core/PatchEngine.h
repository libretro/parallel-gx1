// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace Common
{
class IniFile;
}
namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

// Writes size octets of value to address in RAM, invalidating the instruction cache where it
// changed. With store_existing_value, value is given back what was there.
void ApplyMemoryPatch(const Core::CPUThreadGuard& guard, u8* value, size_t size, const u32 address,
                      bool store_existing_value = true);

template <typename T>
void ApplyMemoryPatch(const Core::CPUThreadGuard& guard, T value, const u32 address)
{
  Common::BigEndianValue<T> big_endian{value};
  ApplyMemoryPatch(guard, reinterpret_cast<u8*>(&big_endian), sizeof(T), address);
}

namespace PatchEngine
{
void LoadPatches();

bool ApplyFramePatches(Core::System& system);
void Shutdown();
void Reload(Core::System& system);
}  // namespace PatchEngine
