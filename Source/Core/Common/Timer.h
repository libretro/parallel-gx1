// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Common
{
class Timer
{
public:
  static u64 NowUs();
  static u64 NowMs();

  void Start();
  // Start(), then decrement start time by the offset.
  // Effectively "resumes" a timer
  void StartWithOffset(u64 offset);
  void Stop();
  u64 ElapsedMs() const;

  // The rest of these functions probably belong somewhere else
  static u64 GetLocalTimeSinceJan1970();

private:
  u64 m_start_ms{0};
  u64 m_end_ms{0};
  bool m_running{false};
};

// Similar to std::chrono::steady_clock except this clock
// specifically does *not* count time while the system is suspended.
class SteadyAwakeClock
{
public:
  using rep = s64;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SteadyAwakeClock>;

  static constexpr bool is_steady = true;

  static time_point now();
};

}  // Namespace Common
