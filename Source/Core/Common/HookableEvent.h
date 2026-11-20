// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdlib>

#include <retro_atomic.h>

#include "Common/CallGate.h"
#include "Common/Logging/Log.h"
#include "Common/Quiesce.h"

namespace Common
{
// The listeners of one event, published as an immutable array, replaced whole on every register
// and unregister, so triggering takes no lock. Counted: the event holds it, and each hook, so a
// hook that outlives its event still unregisters. A callback is a function and its user
// pointer, called as callback(user, args...).
struct HookStorage
{
  struct Listener
  {
    quiesce_node_t node;  // first: the free callbacks cast it back to the whole
    void (*callback)();   // the event's callback type
    void* user;
    call_gate_t gate;
  };

  struct List
  {
    quiesce_node_t node;  // first, as above
    size_t count;
    Listener* items[1];  // count of them
  };

  quiesce_t quiesce;
  retro_atomic_ptr_t list;
  retro_atomic_int_t refs;

  static HookStorage* Make()
  {
    HookStorage* const storage = static_cast<HookStorage*>(std::calloc(1, sizeof(HookStorage)));
    if (storage)
      retro_atomic_store_release_int(&storage->refs, 1);
    return storage;
  }

  void Release()
  {
    if (retro_atomic_fetch_sub_int(&refs, 1) != 1)
      return;
    quiesce_deinit(&quiesce);
    List* const l = static_cast<List*>(retro_atomic_load_acquire_ptr(&list));
    if (l)
    {
      for (size_t i = 0; i != l->count; ++i)
        std::free(l->items[i]);
      std::free(l);
    }
    std::free(this);
  }

  static void FreeNode(quiesce_node_t* node) { std::free(node); }

  // Publishes the current list with remove taken out and add put in.
  void Replace(Listener* remove, Listener* add)
  {
    quiesce_enter(&quiesce);
    for (;;)
    {
      void* cur = retro_atomic_load_acquire_ptr(&list);
      const List* old = static_cast<const List*>(cur);
      const size_t old_count = old ? old->count : 0;
      const size_t count = old_count + (add ? 1 : 0);
      List* next = static_cast<List*>(
          std::malloc(offsetof(List, items) + (count ? count : 1) * sizeof(Listener*)));
      if (!next)
        break;
      next->node = {};
      size_t n = 0;
      for (size_t i = 0; i != old_count; ++i)
      {
        if (old->items[i] != remove)
          next->items[n++] = old->items[i];
      }
      if (add)
        next->items[n++] = add;
      next->count = n;
      if (retro_atomic_cas_ptr(&list, cur, next))
      {
        if (old)
          quiesce_retire(&quiesce, &static_cast<List*>(cur)->node, FreeNode);
        break;
      }
      std::free(next);
    }
    quiesce_exit(&quiesce);
  }
};

// EventHook is a handle a registered listener holds.
// When the handle is destroyed or reset, the listener is removed; one destroyed from inside its
// callback is not called again, and destroying a hook while another thread is inside its
// callback waits for that call to return.
class EventHook
{
public:
  EventHook() = default;
  EventHook(HookStorage* storage, HookStorage::Listener* listener)
      : m_storage(storage), m_listener(listener)
  {
  }
  EventHook(EventHook&& other) noexcept : m_storage(other.m_storage), m_listener(other.m_listener)
  {
    other.m_storage = nullptr;
    other.m_listener = nullptr;
  }
  EventHook& operator=(EventHook&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      m_storage = other.m_storage;
      m_listener = other.m_listener;
      other.m_storage = nullptr;
      other.m_listener = nullptr;
    }
    return *this;
  }
  EventHook(const EventHook&) = delete;
  EventHook& operator=(const EventHook&) = delete;
  ~EventHook() { reset(); }

  void reset()
  {
    if (!m_storage)
      return;
    DEBUG_LOG(COMMON, "Removing event hook handler");
    m_storage->Replace(m_listener, nullptr);
    call_gate_close(&m_listener->gate);
    quiesce_retire(&m_storage->quiesce, &m_listener->node, HookStorage::FreeNode);
    m_storage->Release();
    m_storage = nullptr;
    m_listener = nullptr;
  }

private:
  HookStorage* m_storage = nullptr;
  HookStorage::Listener* m_listener = nullptr;
};

// A hookable event system.
//
// Define Events as:
//
//   HookableEvent<u32> my_lovely_event;
//
// Register listeners anywhere you need them as:
//
//   EventHook my_hook = my_lovely_event.Register(
//       [](void* user, u32 bar) { static_cast<Foo*>(user)->OnBar(bar); }, this);
//
// The hook will be automatically unregistered when the EventHook object goes out of scope.
// Trigger events by calling Trigger as:
//
//   my_lovely_event.Trigger(42);
//
// A hook registered from inside a callback is first called by the next Trigger.
template <typename... CallbackArgs>
class HookableEvent
{
public:
  using CallbackType = void (*)(void* user, CallbackArgs...);

  HookableEvent() : m_storage(HookStorage::Make()) {}
  ~HookableEvent()
  {
    if (m_storage)
      m_storage->Release();
  }
  HookableEvent(const HookableEvent&) = delete;
  HookableEvent& operator=(const HookableEvent&) = delete;

  // Returns a handle that will unregister the listener when destroyed.
  [[nodiscard]] EventHook Register(CallbackType callback, void* user)
  {
    DEBUG_LOG(COMMON, "Registering event hook handler");
    HookStorage::Listener* const listener =
        static_cast<HookStorage::Listener*>(std::calloc(1, sizeof(HookStorage::Listener)));
    if (!m_storage || !listener)
    {
      std::free(listener);
      return EventHook();
    }
    listener->callback = reinterpret_cast<void (*)()>(callback);
    listener->user = user;
    retro_atomic_fetch_add_int(&m_storage->refs, 1);
    m_storage->Replace(nullptr, listener);
    return EventHook(m_storage, listener);
  }

  // Invokes all registered callbacks.
  void Trigger(CallbackArgs... args)
  {
    HookStorage* const storage = m_storage;
    if (!storage)
      return;
    quiesce_enter(&storage->quiesce);
    const HookStorage::List* const list =
        static_cast<const HookStorage::List*>(retro_atomic_load_acquire_ptr(&storage->list));
    if (list)
    {
      for (size_t i = 0; i != list->count; ++i)
      {
        HookStorage::Listener* const listener = list->items[i];
        call_gate_frame_t frame;
        if (!call_gate_enter(&listener->gate, &frame))
          continue;
        reinterpret_cast<CallbackType>(listener->callback)(listener->user, args...);
        call_gate_exit(&listener->gate, &frame);
      }
    }
    quiesce_exit(&storage->quiesce);
  }

private:
  HookStorage* m_storage;
};

}  // namespace Common
