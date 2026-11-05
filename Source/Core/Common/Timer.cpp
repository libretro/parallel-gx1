// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Timer.h"

#include <chrono>
#include "Common/CommonFuncs.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common
{
// Elapsed time is the frontend's: the frame clock, not the host's.
u64 Timer::NowUs()
{
  return frame_clock_time_us();
}

u64 Timer::NowMs()
{
  return frame_clock_time_us() / 1000;
}

void Timer::Start()
{
  m_start_ms = NowMs();
  m_end_ms = 0;
  m_running = true;
}

void Timer::StartWithOffset(u64 offset)
{
  Start();
  m_start_ms -= offset;
}

void Timer::Stop()
{
  m_end_ms = NowMs();
  m_running = false;
}

u64 Timer::ElapsedMs() const
{
  const u64 end = m_running ? NowMs() : m_end_ms;
  // Can handle up to 1 rollover event (underflow produces correct result)
  // If Start() has never been called, will return 0
  return end - m_start_ms;
}

u64 Timer::GetLocalTimeSinceJan1970()
{
#ifdef _MSC_VER
  std::chrono::zoned_seconds seconds(
      std::chrono::current_zone(),
      std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
  return seconds.get_local_time().time_since_epoch().count();
#else
  time_t sysTime, tzDiff, tzDST;
  time(&sysTime);
  tm* gmTime = localtime(&sysTime);

  // Account for DST where needed
  if (gmTime->tm_isdst == 1)
    tzDST = 3600;
  else
    tzDST = 0;

  // Lazy way to get local time in sec
  gmTime = gmtime(&sysTime);
  tzDiff = sysTime - mktime(gmTime);

  return static_cast<u64>(sysTime + tzDiff + tzDST);
#endif
}

}  // Namespace Common
