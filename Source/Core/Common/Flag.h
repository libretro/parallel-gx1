// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Abstraction for a simple flag that can be toggled in a multithreaded way.
//
// Simple API:
// * Set(bool = true): sets the Flag
// * IsSet(): tests if the flag is set
// * Clear(): clears the flag (equivalent to Set(false)).
//
// More advanced features:
// * TestAndSet(bool = true): sets the flag to the given value. If a change was
//                            needed (the flag did not already have this value)
//                            the function returns true. Else, false.
// * TestAndClear(): alias for TestAndSet(false).

#pragma once

#include <retro_atomic.h>

namespace Common
{
class Flag final
{
public:
  // Declared as explicit since we do not want "= true" to work on a flag
  // object - it should be made explicit that a flag is *not* a normal
  // variable.
  explicit Flag(bool initial_value = false) { retro_atomic_int_init(&m_val, initial_value); }
  void Set(bool val = true) { retro_atomic_exchange_int(&m_val, val); }
  void Clear() { Set(false); }
  bool IsSet() const { return retro_atomic_load_seq_cst_int(&m_val) != 0; }
  bool TestAndSet(bool val = true) { return retro_atomic_cas_int(&m_val, !val, val); }

  bool TestAndClear() { return TestAndSet(false); }

  // For waits that take a running flag, such as frame_clock_wait_until.
  const retro_atomic_int_t* Raw() const { return &m_val; }

private:
  mutable retro_atomic_int_t m_val;
};

}  // namespace Common
