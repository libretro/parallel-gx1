// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace Common::JitRegister
{
void Init(const std::string& perf_dir);
void Shutdown();
bool IsEnabled();

/* Names a block of generated code in the perf map. The name is only formatted when a map is open,
 * and must fit on one line. */
void Register(const void* base_address, u32 code_size, const char* format, ...)
    DOLPHIN_PRINTF_ATTR(3, 4);
void Register(const void* start, const void* end, const char* format, ...)
    DOLPHIN_PRINTF_ATTR(3, 4);
}  // namespace Common::JitRegister
