// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef _WIN32
#include <tuple>
#endif

#include <rthreads/rthreads.h>

// Don't include Common.h here as it will break LogManager
#include "Common/CommonTypes.h"

// This may not be defined outside _WIN32
#ifndef _WIN32
#ifndef INFINITE
#define INFINITE 0xffffffff
#endif
#endif

namespace Common
{
int CurrentThreadId();

void SleepCurrentThread(int ms);

// Sleeps for us microseconds; returns at once when us is not positive. Callers that
// sleep until a time point pass the distance to it.
void SleepCurrentThreadUs(long long us);

// Use this function during a spin-wait to make the current thread
// relax while another thread is working. This may be more efficient
// than using events because event functions use kernel calls.
void YieldCPU();

void SetCurrentThreadName(const char* name);

// An sthread entry point that calls F on the object passed as the thread's userdata:
//   m_thread = sthread_create(Common::ThreadEntry<&Foo::Run>, this);
template <auto F>
struct ThreadEntryOf;
template <typename C, void (C::*F)()>
struct ThreadEntryOf<F>
{
  static void Run(void* data) { (static_cast<C*>(data)->*F)(); }
};
template <auto F>
inline void ThreadEntry(void* data)
{
  ThreadEntryOf<F>::Run(data);
}

// Starts a thread nobody joins. Returns false if it couldn't be started.
inline bool StartDetachedThread(void (*entry)(void*), void* data)
{
  sthread_t* const thread = sthread_create(entry, data);
  if (!thread)
    return false;
  sthread_detach(thread);
  return true;
}

// Joins and clears an sthread handle; does nothing for a null one.
inline void JoinThread(sthread_t*& thread)
{
  if (thread)
  {
    sthread_join(thread);
    thread = nullptr;
  }
}

#ifndef _WIN32
// Returns the lowest address of the stack and the size of the stack
std::tuple<void*, size_t> GetCurrentThreadStack();
#endif

}  // namespace Common
