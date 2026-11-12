// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// This header contains type definitions that are shared between the Dolphin core and
// other parts of the code. Any definitions that are only used by the core should be
// placed in "Common.h" instead.

#pragma once

#include <cstdint>

#include "Common/FrameClock.h"

#ifdef _WIN32
#include <tchar.h>
#if defined(__MINGW32__)
#include <windows.h>
#include <string>
#endif
#else
// For using Windows lock code
#define TCHAR char
#define LONG int
#endif

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

/* The core's clock: elapsed time as retro_run has paced it (Common/FrameClock.h), never the
 * host's. It stands still while the frontend pauses and advances a frame at a time. A TimePoint
 * is an instant on it and a DT a span of it, both in microseconds. */
typedef s64 TimePoint;
typedef s64 DT;
struct Clock
{
  static TimePoint now() { return static_cast<TimePoint>(frame_clock_time_us()); }
};

constexpr DT DTFromMs(s64 ms)
{
  return ms * 1000;
}
constexpr DT DTFromSeconds(s64 seconds)
{
  return seconds * 1000000;
}
constexpr s64 DTToMs(DT dt)
{
  return dt / 1000;
}
constexpr double DTToSeconds(DT dt)
{
  return static_cast<double>(dt) / 1000000.0;
}
/* Truncates toward zero, as a duration cast does */
constexpr DT DTFromSecondsF(double seconds)
{
  return static_cast<DT>(seconds * 1000000.0);
}

/* Has the compiler check a printf format (argument @f) against the
 * arguments from @a on. MinGW names the printf its headers picked. */
#if defined(__MINGW32__)
#define DOLPHIN_PRINTF_ATTR(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
#elif defined(__GNUC__)
#define DOLPHIN_PRINTF_ATTR(f, a) __attribute__((format(printf, f, a)))
#else
#define DOLPHIN_PRINTF_ATTR(f, a)
#endif
