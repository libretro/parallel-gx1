// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>

#include <retro_atomic.h>
#include <retro_waitable_spsc.h>
#include <rthreads/retro_eventcount.h>

struct EfbPokeData;
class PointerWrap;

// Requests from the CPU thread to the video thread. The CPU thread is the only producer and the
// video thread the only consumer, so the requests travel through a lock-free SPSC ring as
// pointers: a blocking request lives on the caller's stack and allocates nothing.
class AsyncRequests
{
public:
  typedef void (*Function)(void* data);

  AsyncRequests();
  ~AsyncRequests();

  // Called from the Video thread.
  void PullEvents();

  // The following are called from the CPU thread.

  // Runs fn on a copy of the size bytes at payload, without waiting for it.
  void PushEvent(Function fn, const void* payload, size_t size)
  {
    if (m_passthrough)
    {
      fn(const_cast<void*>(payload));
      return;
    }
    Slot* const slot = NextSlot();
    if (size)
      std::memcpy(slot->payload.bytes, payload, size);
    slot->request.fn = fn;
    slot->request.data = slot->payload.bytes;
    slot->request.done = nullptr;
    Submit(&slot->request);
  }

  // Runs fn(user) and waits for it to return; fn hands back any result through user.
  void PushBlockingEvent(Function fn, void* user)
  {
    if (m_passthrough)
    {
      fn(user);
      return;
    }
    retro_atomic_int_t done;
    Request request;
    retro_atomic_int_init(&done, 0);
    request.fn = fn;
    request.data = user;
    request.done = &done;
    Submit(&request);
    WaitDone(&done);
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  struct Request
  {
    Function fn;
    void* data;
    retro_atomic_int_t* done;  // set by a blocking request: the caller waits on it
  };

  /* Events are built in slots taken in turn, never on the heap: at most
   * QUEUE_REQUESTS wait in the ring and one runs, so a slot comes round
   * again only once the event built in it is done. */
  enum
  {
    QUEUE_REQUESTS = 1024,
    SLOT_COUNT = 2048,
    SLOT_PAYLOAD = 40
  };

  struct Slot
  {
    Request request;
    union
    {
      unsigned long long align;
      double align_double;
      void* align_pointer;
      unsigned char bytes[SLOT_PAYLOAD];
    } payload;
  };

  Slot* NextSlot()
  {
    Slot* const slot = &m_slots[m_next_slot & (SLOT_COUNT - 1)];
    m_next_slot++;
    return slot;
  }

  void Submit(Request* request);
  void WaitDone(retro_atomic_int_t* done);

  static AsyncRequests s_singleton;

  Slot m_slots[SLOT_COUNT];
  unsigned m_next_slot = 0;
  retro_waitable_spsc_t m_queue;
  retro_eventcount_t m_completed;
  bool m_passthrough = true;
};
