// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Multithreaded event class. This allows waiting in a thread for an event to
// be triggered in another thread. While waiting, the CPU will be available for
// other tasks.
// * Set(): triggers the event and wakes up the waiting thread.
// * Wait(): waits for the event to be triggered.
// * Reset(): tries to reset the event before the waiting thread sees it was
//   triggered. Usually a bad idea.

#pragma once


#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

namespace Common
{
// A lock-free auto-reset event: the signal is one atomic flag, and a thread that has to sleep
// parks on an eventcount, so Set() takes no lock and makes no syscall while nobody waits.
class Event final
{
public:
  Event()
  {
    retro_atomic_int_init(&m_flag, 0);
    retro_eventcount_init(&m_wake);
  }
  ~Event() { retro_eventcount_free(&m_wake); }

  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;

  void Set()
  {
    if (!retro_atomic_exchange_int(&m_flag, 1))
      retro_eventcount_notify(&m_wake);
  }

  void Wait()
  {
    while (!TakeSignal())
    {
      const int key = retro_eventcount_prepare_wait(&m_wake);
      if (TakeSignal())
      {
        retro_eventcount_cancel_wait(&m_wake);
        return;
      }
      retro_eventcount_commit_wait(&m_wake, key);
    }
  }

  void Reset()
  {
    // Any lingering signal is dropped; a waiter re-checks the flag on every pass.
    retro_atomic_store_release_int(&m_flag, 0);
  }

private:
  bool TakeSignal()
  {
    return retro_atomic_load_acquire_int(&m_flag) && retro_atomic_exchange_int(&m_flag, 0);
  }

  retro_atomic_int_t m_flag;
  retro_eventcount_t m_wake;
};

}  // namespace Common
