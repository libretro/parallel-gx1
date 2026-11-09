// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once


#include <cstdint>

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
    retro_atomic_int_init(&m_in_run, 0);
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
      // The winner notifies m_done once it is set up.
      for (;;)
      {
        const int key = retro_eventcount_prepare_wait(&m_done);
        if (retro_atomic_load_acquire_int(&m_prepare_state) != PREPARE_BUSY)
        {
          retro_eventcount_cancel_wait(&m_done);
          return;
        }
        retro_eventcount_commit_wait(&m_done, key);
      }
    }

    // so the payload will only be executed once without any Wakeup call
    retro_atomic_exchange_int(&m_running_state, STATE_LAST_EXECUTION);
    m_shutdown.Clear();
    retro_atomic_store_release_int(&m_prepare_state, PREPARE_RUNNING);
    retro_eventcount_notify(&m_done);
  }

  // Main loop of this object.
  // The payload callback is called at least as often as it's needed to match the Wakeup()
  // requirements.
  template <class F>
  void Run(F payload)
  {
    // Asserts that Prepare is called at least once before we enter the loop.
    // But a good implementation should call this before already.
    Prepare();
    retro_atomic_store_release_int(&m_in_run, 1);

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
        // Done: park until new work. Never a busy loop, never a wall-clock deadline.
        if (retro_atomic_fetch_sub_int(&m_running_state, 1) != STATE_DONE)
          break;
        [[fallthrough]];

      case STATE_SLEEPING:
        if (!m_idle_timeout_us)
          m_new_work_event.Wait();
        else if (!m_new_work_event.WaitFor(m_idle_timeout_us))
        {
          // Idle that long: the caller's wedge guard, not a pace.
          m_idle_timed_out = true;
          m_shutdown.Set();
        }
        break;
      }
    }

    // Shutdown down, so get a safe state
    retro_atomic_store_release_int(&m_in_run, 0);
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
      // Prepared but never run: nobody will come along to call Run, so force the stop.
      if (!retro_atomic_load_acquire_int(&m_in_run))
      {
        retro_atomic_store_release_int(&m_prepare_state, PREPARE_STOPPED);
        retro_eventcount_notify(&m_done);
        break;
      }
      Wait();
      break;
    }
  }

  bool IsRunning() const { return !IsStopped() && !m_shutdown.IsSet(); }
  bool IsDone() const
  {
    return IsStopped() || retro_atomic_load_seq_cst_int(&m_running_state) <= STATE_DONE;
  }

  /* The longest Run() parks with no work before it gives up, a wedge guard
   * for a caller that must not wait forever (0: as long as it takes);
   * IdleTimedOut() says whether the last Run() ended that way. Set and read
   * by the thread that calls Run(), around it. */
  void SetIdleTimeout(int64_t timeout_us)
  {
    m_idle_timeout_us = timeout_us;
    m_idle_timed_out = false;
  }
  bool IdleTimedOut() const { return m_idle_timed_out; }

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
  retro_atomic_int_t m_in_run;  // set while Run() is looping
  Flag m_shutdown;                             // If this is set, the loop shall end.
  Event m_new_work_event;
  retro_eventcount_t m_done;

  int64_t m_idle_timeout_us = 0;
  bool m_idle_timed_out = false;
};

}  // namespace Common
