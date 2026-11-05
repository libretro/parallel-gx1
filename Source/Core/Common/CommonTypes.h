// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// This header contains type definitions that are shared between the Dolphin core and
// other parts of the code. Any definitions that are only used by the core should be
// placed in "Common.h" instead.

#pragma once

#include <chrono>
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

// The core's clock: elapsed time as retro_run has paced it (Common/FrameClock.h), never the
// host's. It stands still while the frontend pauses and advances a frame at a time.
struct FrameTimeClock
{
  using rep = s64;
  using period = std::micro;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<FrameTimeClock>;
  static constexpr bool is_steady = true;
  static time_point now() { return time_point(duration(static_cast<rep>(frame_clock_time_us()))); }
};

using Clock = FrameTimeClock;
using TimePoint = Clock::time_point;
using DT = Clock::duration;
using DT_us = std::chrono::duration<double, std::micro>;
using DT_ms = std::chrono::duration<double, std::milli>;
using DT_s = std::chrono::duration<double, std::ratio<1>>;
