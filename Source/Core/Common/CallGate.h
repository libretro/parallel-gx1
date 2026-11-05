/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_CALLGATE_H
#define DOLPHIN_COMMON_CALLGATE_H

#include <boolean.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lets a callback be unregistered while other threads may be calling
 * it, without a lock on the calling path. A caller passes the gate to
 * enter; closing it shuts out new callers and returns once the ones
 * already inside have left, so the owner can then free what the
 * callback captured. A callback closing its own gate from inside does
 * not wait on itself.
 *
 * A zero-filled gate is open and empty; it needs no init or deinit. */
typedef struct call_gate
{
   retro_atomic_int_t calls;
   retro_atomic_int_t closed;
} call_gate_t;

/* One per entry, on the caller's stack, from call_gate_enter until
 * the matching call_gate_exit: it records which gates the thread is
 * inside. */
typedef struct call_gate_frame
{
   call_gate_t *gate;
   struct call_gate_frame *prev;
} call_gate_frame_t;

/* False when the gate is closed; the callback must not run then and
 * the frame is unused. Pair a true with call_gate_exit. */
bool call_gate_enter(call_gate_t *gate, call_gate_frame_t *frame);
void call_gate_exit(call_gate_t *gate, call_gate_frame_t *frame);
void call_gate_close(call_gate_t *gate);
/* Returns once the calls inside now have left, leaving the gate open:
 * for when the caller has already unpublished what those calls might
 * be using. */
void call_gate_drain(call_gate_t *gate);
/* Undoes one call_gate_close; the gate lets calls in again once every
 * close is undone. */
void call_gate_reopen(call_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif
