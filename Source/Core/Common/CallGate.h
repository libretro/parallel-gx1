// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <retro_atomic.h>

namespace Common
{
// Lets a callback be unregistered while other threads may be calling it, without a lock on
// the calling path. A caller passes the gate to enter; closing it shuts out new callers and
// returns once the ones already inside have left, so the owner can then free what the
// callback captured. A callback closing its own gate from inside does not wait on itself.
struct CallGate
{
  retro_atomic_int_t calls{};
  retro_atomic_int_t closed{};
};

// False when the gate is closed; the callback must not run then. Pair a true with CallGateExit.
bool CallGateEnter(CallGate* gate);
void CallGateExit(CallGate* gate);
void CallGateClose(CallGate* gate);
// Returns once the calls inside now have left, leaving the gate open: for when the caller
// has already unpublished what those calls might be using.
void CallGateDrain(CallGate* gate);
// Undoes one CallGateClose; the gate lets calls in again once every close is undone.
void CallGateReopen(CallGate* gate);
}  // namespace Common
