// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include <retro_atomic.h>

#include "Common/CallGate.h"
#include "Common/Logging/Log.h"
#include "Common/Quiesce.h"

namespace Common
{
struct HookBase
{
  // A pure virtual destructor makes this class abstract to prevent accidental "slicing".
  virtual ~HookBase() = 0;
};
inline HookBase::~HookBase() = default;

// EventHook is a handle a registered listener holds.
// When the handle is destroyed, the HookableEvent will automatically remove the listener.
// If the handle outlives the HookableEvent, the link will be properly disconnected.
using EventHook = std::unique_ptr<HookBase>;

// A hookable event system.
//
// Define Events as:
//
//   HookableEvent<std::string, u32> my_lovely_event;
//
// Register listeners anywhere you need them as:
//
//   EventHook my_hook = my_lovely_event.Register([](std::string foo, u32 bar) {
//       fmt::print("I've been triggered with {} and {}", foo, bar)
//   });
//
// The hook will be automatically unregistered when the EventHook object goes out of scope.
// Trigger events by calling Trigger as:
//
//   my_lovely_event.Trigger("Hello world", 42);
//

// Listeners are published as an immutable array, replaced whole on every register and
// unregister, so triggering takes no lock. A hook registered from inside a callback is first
// called by the next Trigger; one destroyed from inside is not called again, and destroying a
// hook while another thread is inside its callback waits for that call to return.
template <typename... CallbackArgs>
class HookableEvent
{
public:
  using CallbackType = std::function<void(CallbackArgs...)>;

  // Returns a handle that will unregister the listener when destroyed.
  [[nodiscard]] EventHook Register(CallbackType callback)
  {
    DEBUG_LOG_FMT(COMMON, "Registering event hook handler");
    auto* listener = new Listener{{}, std::move(callback), {}};
    m_storage->Replace(nullptr, listener);
    return std::make_unique<HookImpl>(m_storage, listener);
  }

  // Invokes all registered callbacks.
  void Trigger(const CallbackArgs&... args)
  {
    Storage* const storage = m_storage.get();
    quiesce_enter(&storage->quiesce);
    if (const auto* list = static_cast<const List*>(retro_atomic_load_acquire_ptr(&storage->list)))
    {
      for (std::size_t i = 0; i != list->count; ++i)
      {
        Listener* const listener = list->items[i];
        if (!Common::CallGateEnter(&listener->gate))
          continue;
        std::invoke(listener->callback, args...);
        Common::CallGateExit(&listener->gate);
      }
    }
    quiesce_exit(&storage->quiesce);
  }

private:
  // node first: the free callbacks cast it back to the whole.
  struct Listener
  {
    quiesce_node_t node;
    const CallbackType callback;
    Common::CallGate gate;

    static void Free(quiesce_node_t* node) { delete reinterpret_cast<Listener*>(node); }
  };

  struct List
  {
    quiesce_node_t node;
    std::size_t count;
    Listener* items[1];  // count of them

    static List* Make(std::size_t count)
    {
      const std::size_t n = count ? count : 1;
      void* mem = ::operator new(offsetof(List, items) + n * sizeof(Listener*));
      List* list = static_cast<List*>(mem);
      list->node = {};
      list->count = count;
      return list;
    }
    static void Free(quiesce_node_t* node) { ::operator delete(node); }
  };

  struct Storage
  {
    quiesce_t quiesce{};
    retro_atomic_ptr_t list{};

    ~Storage()
    {
      quiesce_deinit(&quiesce);
      if (auto* l = static_cast<List*>(retro_atomic_load_acquire_ptr(&list)))
      {
        for (std::size_t i = 0; i != l->count; ++i)
          delete l->items[i];
        List::Free(&l->node);
      }
    }

    // Publishes the current list with remove taken out and add put in.
    void Replace(Listener* remove, Listener* add)
    {
      quiesce_enter(&quiesce);
      for (;;)
      {
        void* cur = retro_atomic_load_acquire_ptr(&list);
        const List* old = static_cast<const List*>(cur);
        const std::size_t old_count = old ? old->count : 0;
        List* next = List::Make(old_count + (add ? 1 : 0));
        std::size_t n = 0;
        for (std::size_t i = 0; i != old_count; ++i)
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
            quiesce_retire(&quiesce, &static_cast<List*>(cur)->node, List::Free);
          break;
        }
        List::Free(&next->node);
      }
      quiesce_exit(&quiesce);
    }
  };

  struct HookImpl final : HookBase
  {
    HookImpl(std::weak_ptr<Storage> storage, Listener* listener)
        : weak_storage{std::move(storage)}, listener_ptr{listener}
    {
    }

    ~HookImpl() override
    {
      const auto storage = weak_storage.lock();
      if (storage == nullptr)
      {
        DEBUG_LOG_FMT(COMMON, "Handler outlived event hook");
        return;
      }

      DEBUG_LOG_FMT(COMMON, "Removing event hook handler");

      storage->Replace(listener_ptr, nullptr);
      Common::CallGateClose(&listener_ptr->gate);
      quiesce_retire(&storage->quiesce, &listener_ptr->node, Listener::Free);
    }

    const std::weak_ptr<Storage> weak_storage;
    Listener* const listener_ptr;  // "owned" by the above Storage.
  };

  // shared_ptr storage allows hooks to forget their connection if they outlive the event itself.
  std::shared_ptr<Storage> m_storage{std::make_shared<Storage>()};
};

}  // namespace Common
