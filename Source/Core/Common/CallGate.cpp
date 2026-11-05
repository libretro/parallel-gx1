// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CallGate.h"

#include <rthreads/retro_eventcount.h>

namespace Common
{
namespace
{
// Gates this thread is inside, innermost last, so a close from within knows its own calls.
constexpr int MAX_NESTED = 16;
thread_local CallGate* t_inside[MAX_NESTED];
thread_local int t_depth;

retro_eventcount_t* Left()
{
  static retro_eventcount_t ec;
  static const bool init = retro_eventcount_init(&ec);
  (void)init;
  return &ec;
}

int OwnCalls(const CallGate* gate)
{
  int own = 0;
  for (int i = 0; i < t_depth && i < MAX_NESTED; ++i)
    own += t_inside[i] == gate;
  return own;
}
}  // namespace

bool CallGateEnter(CallGate* gate)
{
  // Counted before the check, the closer's store before its read: either the closer sees
  // this call or this sees the gate closed.
  retro_atomic_fetch_add_seq_cst_int(&gate->calls, 1);
  if (retro_atomic_load_seq_cst_int(&gate->closed))
  {
    retro_atomic_fetch_add_seq_cst_int(&gate->calls, -1);
    retro_eventcount_notify(Left());
    return false;
  }
  if (t_depth < MAX_NESTED)
    t_inside[t_depth] = gate;
  ++t_depth;
  return true;
}

void CallGateExit(CallGate* gate)
{
  // Calls nest, so the innermost entry is this one.
  --t_depth;
  retro_atomic_fetch_add_seq_cst_int(&gate->calls, -1);
  retro_eventcount_notify(Left());
}

void CallGateClose(CallGate* gate)
{
  retro_atomic_fetch_add_seq_cst_int(&gate->closed, 1);
  const int own = OwnCalls(gate);
  for (;;)
  {
    if (retro_atomic_load_seq_cst_int(&gate->calls) <= own)
      return;
    const int key = retro_eventcount_prepare_wait(Left());
    if (retro_atomic_load_seq_cst_int(&gate->calls) <= own)
    {
      retro_eventcount_cancel_wait(Left());
      return;
    }
    retro_eventcount_commit_wait(Left(), key);
  }
}
}  // namespace Common
