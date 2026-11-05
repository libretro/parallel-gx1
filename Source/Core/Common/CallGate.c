/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/CallGate.h"

#include <stdlib.h>

#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

/* Both live for the whole process. Each is made on first use; threads
 * that race build one apiece and the CAS keeps the first, so nobody
 * waits on another. */
static retro_atomic_ptr_t s_left; /* retro_eventcount_t*: a call left */
static retro_atomic_ptr_t s_key;  /* sthread_tls_t*: top frame of this thread */

static retro_eventcount_t *call_gate_left(void)
{
   retro_eventcount_t *ec = (retro_eventcount_t*)retro_atomic_load_acquire_ptr(&s_left);
   if (!ec)
   {
      retro_eventcount_t *fresh = (retro_eventcount_t*)calloc(1, sizeof(*fresh));
      retro_eventcount_init(fresh);
      if (retro_atomic_cas_ptr(&s_left, NULL, fresh))
         return fresh;
      retro_eventcount_free(fresh);
      free(fresh);
      ec = (retro_eventcount_t*)retro_atomic_load_acquire_ptr(&s_left);
   }
   return ec;
}

static sthread_tls_t *call_gate_key(void)
{
   sthread_tls_t *key = (sthread_tls_t*)retro_atomic_load_acquire_ptr(&s_key);
   if (!key)
   {
      sthread_tls_t *fresh = (sthread_tls_t*)calloc(1, sizeof(*fresh));
      sthread_tls_create(fresh);
      if (retro_atomic_cas_ptr(&s_key, NULL, fresh))
         return fresh;
      sthread_tls_delete(fresh);
      free(fresh);
      key = (sthread_tls_t*)retro_atomic_load_acquire_ptr(&s_key);
   }
   return key;
}

/* How many times this thread is inside gate. */
static int call_gate_own(const call_gate_t *gate)
{
   int own = 0;
   const call_gate_frame_t *frame =
      (const call_gate_frame_t*)sthread_tls_get(call_gate_key());
   for (; frame; frame = frame->prev)
      own += frame->gate == gate;
   return own;
}

bool call_gate_enter(call_gate_t *gate, call_gate_frame_t *frame)
{
   sthread_tls_t *key;
   /* Counted before the check, the closer's store before its read:
    * either the closer sees this call or this sees the gate closed. */
   retro_atomic_fetch_add_seq_cst_int(&gate->calls, 1);
   /* Orders the count before every later load of the caller's, not
    * just the one below, so a call_gate_drain that unpublished
    * something first is seen to. */
   retro_atomic_thread_fence_seq_cst();
   if (retro_atomic_load_seq_cst_int(&gate->closed))
   {
      retro_atomic_fetch_add_seq_cst_int(&gate->calls, -1);
      retro_eventcount_notify(call_gate_left());
      return false;
   }
   key         = call_gate_key();
   frame->gate = gate;
   frame->prev = (call_gate_frame_t*)sthread_tls_get(key);
   sthread_tls_set(key, frame);
   return true;
}

void call_gate_exit(call_gate_t *gate, call_gate_frame_t *frame)
{
   /* Calls nest, so the innermost entry is this one. */
   sthread_tls_set(call_gate_key(), frame->prev);
   retro_atomic_fetch_add_seq_cst_int(&gate->calls, -1);
   retro_eventcount_notify(call_gate_left());
}

void call_gate_close(call_gate_t *gate)
{
   retro_atomic_fetch_add_seq_cst_int(&gate->closed, 1);
   call_gate_drain(gate);
}

void call_gate_drain(call_gate_t *gate)
{
   int own;
   retro_eventcount_t *left = call_gate_left();
   retro_atomic_thread_fence_seq_cst();
   own = call_gate_own(gate);
   for (;;)
   {
      int key;
      if (retro_atomic_load_seq_cst_int(&gate->calls) <= own)
         return;
      key = retro_eventcount_prepare_wait(left);
      if (retro_atomic_load_seq_cst_int(&gate->calls) <= own)
      {
         retro_eventcount_cancel_wait(left);
         return;
      }
      retro_eventcount_commit_wait(left, key);
   }
}

void call_gate_reopen(call_gate_t *gate)
{
   retro_atomic_fetch_add_seq_cst_int(&gate->closed, -1);
}
