/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_FRAMECLOCK_H
#define DOLPHIN_COMMON_FRAMECLOCK_H

#include <boolean.h>
#include <stdint.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The core's only timebase outside emulated time: the count of
 * retro_run calls. Background threads that used to act every so many
 * milliseconds wait for a number of frames instead, so they run at
 * the frontend's pace, stop while it pauses, and never sleep or read a
 * clock. */

/* Called once per retro_run, by the frontend's thread, with the length
 * of a frame at the refresh rate the core reports. */
void frame_clock_advance(unsigned frame_us);

uint64_t frame_clock_now(void);

/* The sum of the frame lengths so far, in microseconds: the core's
 * idea of elapsed time, paced by the frontend. */
uint64_t frame_clock_time_us(void);

/* Blocks until frame target has begun or *running reads zero. running
 * may be NULL. Returns false when no longer running. Whoever clears
 * *running calls frame_clock_wake_all afterwards. */
bool frame_clock_wait_until(uint64_t target, const retro_atomic_int_t *running);

/* Wakes every waiter to look at its running flag again. */
void frame_clock_wake_all(void);

/* For a wait on frames and something else at once: the other thing's
 * producer calls frame_clock_wake_all. Same protocol as
 * retro_eventcount: prepare, check, then cancel or commit. */
int frame_clock_prepare_wait(void);
void frame_clock_cancel_wait(void);
void frame_clock_commit_wait(int key);

#ifdef __cplusplus
}
#endif

#endif
