// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <ctime>
#include <optional>

namespace Common
{
// Threadsafe and error-checking variant of std::localtime()
std::optional<std::tm> LocalTime(std::time_t time);

// Threadsafe std::gmtime(): fills @out, false when @time cannot be converted.
bool GmTime(std::time_t time, std::tm* out);
}  // Namespace Common
