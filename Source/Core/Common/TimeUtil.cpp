// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/TimeUtil.h"

#include <cstring>
#include <ctime>
#include <optional>

#include "Common/Logging/Log.h"

namespace Common
{
std::optional<std::tm> LocalTime(std::time_t time)
{
  std::tm local_time;
#ifdef _WIN32
  if (localtime_s(&local_time, &time) != 0)
#else
  if (localtime_r(&time, &local_time) == nullptr)
#endif
  {
    ERROR_LOG(COMMON, "Failed to convert time to local time: %s", std::strerror(errno));
    return std::nullopt;
  }
  return local_time;
}

bool GmTime(std::time_t time, std::tm* out)
{
#ifdef _WIN32
  return gmtime_s(out, &time) == 0;
#else
  return gmtime_r(&time, out) != nullptr;
#endif
}
}  // Namespace Common
