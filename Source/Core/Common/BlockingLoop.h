// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "Common/Event.h"
#include "Common/Flag.h"

namespace Common
{
// This class provides a synchronized loop.
// It's a thread-safe way to trigger a new iteration without busy loops.
// It's optimized for high-usage iterations which usually are already running while it's triggered
// often.
// Be careful when using Wait() and Wakeup() at the same time. Wait() may block forever while
// Wakeup() is called regularly.
//
// No locks: the run state is one atomic, the loop thread sleeps on an Event, and any number of
// threads may wait for it to finish through an eventcount, each re-checking IsDone().
class BlockingLoop
{
public:
  enum class StopMode
  {
    NonBlock,
    Block,
    BlockAndGiveUp,
  };

  BlockingLoop()
  {
    retro_atomic_int_init(&m_running_state, STATE_DONE);
    retro_atomic_int_init(&m_prepare_state, PREPARE_STOPPED);
    retro_eventcount_init(&m_done);
  }
  ~BlockingLoop()
  {
    Stop(StopMode::BlockAndGiveUp);
    retro_eventcount_free(&m_done);
  }

  BlockingLoop(const BlockingLoop&) = delete;
  BlockingLoop& operator=(const BlockingLoop&) = delete;

  // Triggers to rerun the payload of the Run() function at least once again.
  // This function will never block and is designed to finish as fast as possible.
  void Wakeup()
  {
    // Already running, so no need for a wakeup.
    // This is the common case, so try to get this as fast as possible.
    if (retro_atomic_load_seq_cst_int(&m_running_state) >= STATE_NEED_EXECUTION)
      return;

    // Mark that new data is available. If the old state will rerun the payload
    // itself, we don't have to set the event to interrupt the worker.
    if (retro_atomic_exchange_int(&m_running_state, STATE_NEED_EXECUTION) != STATE_SLEEPING)
      return;

    // Else as the worker thread may sleep now, we have to set the event.
    m_new_work_event.Set();
  }

  // Wait for a complete payload run after the last Wakeup() call.
  // If stopped, this returns immediately.
  void Wait()
  {
    while (!IsDone())
    {
      const int key = retro_eventcount_prepare_wait(&m_done);
      if (IsDone())
      {
        retro_eventcount_cancel_wait(&m_done);
        break;
      }
      retro_eventcount_commit_wait(&m_done, key);
    }

    // As we wanted to wait for the other thread, there is likely no work remaining.
    // So there is no need for a busy loop any more.
    m_may_sleep.Set();
  }

  // Wait for a complete payload run after the last Wakeup() call.
  // This version will call a yield function every 100ms.
  // If stopped, this returns immediately.
  template <class Rep, class Period, typename Functor>
  void WaitYield(const std::chrono::duration<Rep, Period>& rel_time, Functor yield_func)
  {
    const int64_t timeout_us =
        std::chrono::duration_cast<std::chrono::microseconds>(rel_time).count();
    while (!IsDone())
    {
      const int key = retro_eventcount_prepare_wait(&m_done);
      if (IsDone())
      {
        retro_eventcount_cancel_wait(&m_done);
        break;
      }
      if (!retro_eventcount_commit_wait_timeout(&m_done, key, timeout_us))
        yield_func();
    }

    // As we wanted to wait for the other thread, there is likely no work remaining.
    // So there is no need for a busy loop any more.
    m_may_sleep.Set();
  }

  // Half start the worker.
  // So this object is in a running state and Wait() will block until the worker calls Run().
  // This may be called from any thread and is supposed to be called at least once before Wait()
  // is used.
  void Prepare()
  {
    // There is a race condition if the other threads call this function while
    // the loop thread is initializing. The thread that moves the loop out of the stopped state
    // sets it up; any other one waits for that, which is a few stores.
    if (!retro_atomic_cas_int(&m_prepare_state, PREPARE_STOPPED, PREPARE_BUSY))
    {
      while (retro_atomic_load_acquire_int(&m_prepare_state) == PREPARE_BUSY)
        sthread_yield();
      return;
    }

    // so the payload will only be executed once without any Wakeup call
    retro_atomic_exchange_int(&m_running_state, STATE_LAST_EXECUTION);
    m_shutdown.Clear();
    m_may_sleep.Set();
    retro_atomic_store_release_int(&m_prepare_state, PREPARE_RUNNING);
  }

  // Main loop of this object.
  // The payload callback is called at least as often as it's needed to match the Wakeup()
  // requirements.
  // The optional timeout parameter is a timeout for how periodically the payload should be called.
  // Use timeout = 0 to run without a timeout at all.
  template <class F>
  void Run(F payload, int64_t timeout = 0)
  {
    // Asserts that Prepare is called at least once before we enter the loop.
    // But a good implementation should call this before already.
    Prepare();

    while (!m_shutdown.IsSet())
    {
      payload();

      switch (retro_atomic_load_seq_cst_int(&m_running_state))
      {
      case STATE_NEED_EXECUTION:
        // We won't get notified while we are in the STATE_NEED_EXECUTION state, so maybe Wakeup
        // was called. So we have to assume on finishing the STATE_NEED_EXECUTION state, that there
        // may be some remaining tasks. To process this tasks, we call the payload again within
        // the STATE_LAST_EXECUTION state.
        retro_atomic_fetch_sub_int(&m_running_state, 1);
        break;

      case STATE_LAST_EXECUTION:
        // If we're still in the STATE_LAST_EXECUTION state, then Wakeup wasn't called within the
        // last execution of the payload. This means we should be ready now. But bad luck, Wakeup
        // may have been called right now. So break and rerun the payload if the state was touched.
        if (retro_atomic_fetch_sub_int(&m_running_state, 1) != STATE_LAST_EXECUTION)
          break;

        // Else we're likely in the STATE_DONE state now, so wakeup the waiting threads right now.
        // However, if we're not in the STATE_DONE state any more, the event should also be
        // triggered so that we'll skip the next waiting call quite fast.
        retro_eventcount_notify(&m_done);
        [[fallthrough]];

      case STATE_DONE:
        // We're done now. So time to check if we want to sleep or if we want to stay in a busy
        // loop.
        if (m_may_sleep.TestAndClear())
        {
          // Try to set the sleeping state.
          if (retro_atomic_fetch_sub_int(&m_running_state, 1) != STATE_DONE)
            break;
        }
        else
        {
          // Busy loop.
          break;
        }
        [[fallthrough]];

      case STATE_SLEEPING:
        // Just relax
        if (timeout > 0)
        {
          m_new_work_event.WaitFor(std::chrono::milliseconds(timeout));
        }
        else
        {
          m_new_work_event.Wait();
        }
        break;
      }
    }

    // Shutdown down, so get a safe state
    retro_atomic_exchange_int(&m_running_state, STATE_DONE);
    retro_atomic_store_release_int(&m_prepare_state, PREPARE_STOPPED);

    // Wake up the last Wait calls.
    retro_eventcount_notify(&m_done);
  }

  // Quits the main loop.
  // By default, it will wait until the main loop quits.
  // Be careful to not use the blocking way within the payload of the Run() method.
  void Stop(StopMode mode = StopMode::Block)
  {
    if (IsStopped())
      return;

    m_shutdown.Set();
    Wakeup();

    switch (mode)
    {
    case StopMode::NonBlock:
      break;
    case StopMode::Block:
      Wait();
      break;
    case StopMode::BlockAndGiveUp:
      WaitYield(std::chrono::milliseconds(100), [&] {
        // If timed out, assume no one will come along to call Run, so force a break
        retro_atomic_store_release_int(&m_prepare_state, PREPARE_STOPPED);
      });
      break;
    }
  }

  bool IsRunning() const { return !IsStopped() && !m_shutdown.IsSet(); }
  bool IsDone() const
  {
    return IsStopped() || retro_atomic_load_seq_cst_int(&m_running_state) <= STATE_DONE;
  }

  // This function should be triggered by regularly by time. So we will fall back from
  // the busy loop to the sleeping way.
  void AllowSleep() { m_may_sleep.Set(); }

private:
  bool IsStopped() const
  {
    return retro_atomic_load_acquire_int(&m_prepare_state) == PREPARE_STOPPED;
  }

  enum RUNNING_TYPE
  {
    // These states are managed by the Run() method
    STATE_SLEEPING = 0,
    STATE_DONE = 1,
    STATE_LAST_EXECUTION = 2,
    // This state is managed by the Wakeup() method
    STATE_NEED_EXECUTION = 3
  };

  enum PREPARE_TYPE
  {
    PREPARE_STOPPED = 0,  // Wait() shall not block.
    PREPARE_BUSY = 1,     // One thread is setting the loop up.
    PREPARE_RUNNING = 2,
  };

  mutable retro_atomic_int_t m_running_state;  // must be of type RUNNING_TYPE
  mutable retro_atomic_int_t m_prepare_state;  // must be of type PREPARE_TYPE
  Flag m_shutdown;                             // If this is set, the loop shall end.
  Event m_new_work_event;
  retro_eventcount_t m_done;

  // If this is set, we fall back from the busy loop to an event based synchronization.
  // This variable is set to true after a timeout or if Wait() is called.
  Flag m_may_sleep;
};

}  // namespace Common
