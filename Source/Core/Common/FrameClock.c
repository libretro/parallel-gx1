/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/FrameClock.h"

#include <stdlib.h>

#include <rthreads/retro_eventcount.h>

static retro_atomic_64_t  s_frame;
static retro_atomic_64_t  s_time_us;
static retro_atomic_ptr_t s_tick; /* retro_eventcount_t*, made on first use */

static retro_eventcount_t *frame_clock_tick(void)
{
   retro_eventcount_t *ec = (retro_eventcount_t*)retro_atomic_load_acquire_ptr(&s_tick);
   if (!ec)
   {
      /* Racing threads build one apiece; the CAS keeps the first. */
      retro_eventcount_t *fresh = (retro_eventcount_t*)calloc(1, sizeof(*fresh));
      retro_eventcount_init(fresh);
      if (retro_atomic_cas_ptr(&s_tick, NULL, fresh))
         return fresh;
      retro_eventcount_free(fresh);
      free(fresh);
      ec = (retro_eventcount_t*)retro_atomic_load_acquire_ptr(&s_tick);
   }
   return ec;
}

void frame_clock_advance(unsigned frame_us)
{
   retro_atomic_fetch_add_64(&s_time_us, (int64_t)frame_us);
   retro_atomic_fetch_add_64(&s_frame, 1);
   retro_eventcount_notify(frame_clock_tick());
}

uint64_t frame_clock_now(void)
{
   return (uint64_t)retro_atomic_load_acquire_64(&s_frame);
}

uint64_t frame_clock_time_us(void)
{
   return (uint64_t)retro_atomic_load_acquire_64(&s_time_us);
}

bool frame_clock_wait_until(uint64_t target, const retro_atomic_int_t *running)
{
   retro_eventcount_t *tick = frame_clock_tick();
   for (;;)
   {
      int key;
      if (running && !retro_atomic_load_acquire_int((retro_atomic_int_t*)running))
         return false;
      if (frame_clock_now() >= target)
         return true;
      key = retro_eventcount_prepare_wait(tick);
      if ((running && !retro_atomic_load_acquire_int((retro_atomic_int_t*)running))
            || frame_clock_now() >= target)
      {
         retro_eventcount_cancel_wait(tick);
         continue;
      }
      retro_eventcount_commit_wait(tick, key);
   }
}

void frame_clock_wake_all(void)
{
   retro_eventcount_notify(frame_clock_tick());
}

int frame_clock_prepare_wait(void)
{
   return retro_eventcount_prepare_wait(frame_clock_tick());
}

void frame_clock_cancel_wait(void)
{
   retro_eventcount_cancel_wait(frame_clock_tick());
}

void frame_clock_commit_wait(int key)
{
   retro_eventcount_commit_wait(frame_clock_tick(), key);
}
