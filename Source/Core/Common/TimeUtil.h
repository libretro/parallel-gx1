// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <ctime>

namespace Common
{
// Threadsafe and error-checking std::localtime(): fills @local_time, false when @time cannot be
// converted.
bool LocalTime(std::time_t time, std::tm* local_time);

// Threadsafe std::gmtime(): fills @out, false when @time cannot be converted.
bool GmTime(std::time_t time, std::tm* out);
}  // Namespace Common
