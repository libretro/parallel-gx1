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
template <typename Clock, typename Duration>
static typename Clock::rep time_now()
{
  return std::chrono::time_point_cast<Duration>(Clock::now()).time_since_epoch().count();
}

template <typename Duration>
static auto steady_time_now()
{
  return time_now<std::chrono::steady_clock, Duration>();
}

u64 Timer::NowUs()
{
  return steady_time_now<std::chrono::microseconds>();
}

u64 Timer::NowMs()
{
  return steady_time_now<std::chrono::milliseconds>();
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

// Results are appropriately slewed on Linux, but not on Windows, macOS, or FreeBSD.
// Clocks with that functionality seem to not be available there.
auto SteadyAwakeClock::now() -> time_point
{
#if defined(_WIN32)
  // The count is system time "in units of 100 nanoseconds".
  using InterruptDuration = std::chrono::duration<ULONGLONG, std::ratio<100, std::nano::den>::type>;

  ULONGLONG interrupt_time{};
  if (!QueryUnbiasedInterruptTime(&interrupt_time))
    ERROR_LOG_FMT(COMMON, "QueryUnbiasedInterruptTime");

  return time_point{InterruptDuration{interrupt_time}};
#else
  // Note that Linux's CLOCK_MONOTONIC "does not count time that the system is suspended".
  // This is in contrast to the behavior on macOS and FreeBSD.
  static constexpr auto clock_id =
#if defined(__linux__)
      CLOCK_MONOTONIC;
#elif defined(__APPLE__)
      CLOCK_UPTIME_RAW;
#else
      CLOCK_UPTIME;
#endif

  timespec ts{};
  if (clock_gettime(clock_id, &ts) != 0)
    ERROR_LOG_FMT(COMMON, "clock_gettime: {}", LastStrerrorString());

  return time_point{std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}};
#endif
}

}  // Namespace Common
