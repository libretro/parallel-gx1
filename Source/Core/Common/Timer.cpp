// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Timer.h"

#include <ctime>
#include "Common/CommonFuncs.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <sys/time.h>
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
}

u64 Timer::WallClockUs()
{
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const u64 ticks = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  /* 100 ns ticks since 1601-01-01 */
  return (ticks - 116444736000000000ULL) / 10;
#else
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<u64>(tv.tv_sec) * 1000000 + static_cast<u64>(tv.tv_usec);
#endif
}

}  // Namespace Common
