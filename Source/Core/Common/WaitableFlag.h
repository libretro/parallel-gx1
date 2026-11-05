// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

namespace Common
{
// A flag other threads can wait to see at a given value. Setting it is one atomic exchange,
// plus a wake only when the value changed; waiters park on an eventcount.
class WaitableFlag final
{
public:
  WaitableFlag()
  {
    retro_atomic_int_init(&m_flag, 0);
    retro_eventcount_init(&m_changed);
  }
  ~WaitableFlag() { retro_eventcount_free(&m_changed); }

  WaitableFlag(const WaitableFlag&) = delete;
  WaitableFlag& operator=(const WaitableFlag&) = delete;

  void Set(bool value = true)
  {
    if (retro_atomic_exchange_int(&m_flag, value ? 1 : 0) != (value ? 1 : 0))
      retro_eventcount_notify(&m_changed);
  }

  void Reset() { Set(false); }

  void Wait(bool expected_value)
  {
    for (;;)
    {
      if (Is(expected_value))
        return;
      const int key = retro_eventcount_prepare_wait(&m_changed);
      if (Is(expected_value))
      {
        retro_eventcount_cancel_wait(&m_changed);
        return;
      }
      retro_eventcount_commit_wait(&m_changed, key);
    }
  }

private:
  bool Is(bool value) { return (retro_atomic_load_acquire_int(&m_flag) != 0) == value; }

  retro_atomic_int_t m_flag;
  retro_eventcount_t m_changed;
};

}  // namespace Common
