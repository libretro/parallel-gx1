// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

#include <retro_atomic.h>

#include "Common/Quiesce.h"

namespace Common
{
// A map any thread may read and write without a lock, for keys that are only ever added (a
// value can be replaced or removed, the key's slot stays). Each bucket is a list pushed at
// its head with a CAS; a value is a published copy, swapped whole and freed once no reader
// is inside the map's quiesce.
template <typename K, typename V, size_t BUCKETS = 4096, typename Hash = std::hash<K>>
class AtomicHashMap final
{
public:
  AtomicHashMap() = default;
  AtomicHashMap(const AtomicHashMap&) = delete;
  AtomicHashMap& operator=(const AtomicHashMap&) = delete;

  ~AtomicHashMap()
  {
    for (auto& bucket : m_buckets)
    {
      auto* node = static_cast<Node*>(retro_atomic_load_acquire_ptr(&bucket));
      while (node)
      {
        Node* const next = node->next;
        delete static_cast<Value*>(retro_atomic_load_acquire_ptr(&node->value));
        delete node;
        node = next;
      }
    }
    quiesce_deinit(&m_quiesce);
  }

  // A copy of the value for key, if it has one.
  template <typename Key>
  std::optional<V> Get(const Key& key) const
  {
    quiesce_enter(&m_quiesce);
    std::optional<V> result;
    if (const Node* node = Find(Bucket(key), key, nullptr))
    {
      if (const auto* value = static_cast<const Value*>(retro_atomic_load_acquire_ptr(&node->value)))
        result = value->value;
    }
    quiesce_exit(&m_quiesce);
    return result;
  }

  void Set(const K& key, V value) { Replace(key, new Value{{}, std::move(value)}); }

  template <typename Key>
  void Erase(const Key& key)
  {
    quiesce_enter(&m_quiesce);
    if (Node* node = Find(Bucket(key), key, nullptr))
      Retire(retro_atomic_exchange_ptr(&node->value, nullptr));
    quiesce_exit(&m_quiesce);
  }

private:
  struct Value
  {
    quiesce_node_t node;  // first: the free callback casts it back
    V value;
  };
  struct Node
  {
    Node* next;  // fixed once published
    K key;
    retro_atomic_ptr_t value;  // Value*, or null when removed
  };

  template <typename Key>
  retro_atomic_ptr_t* Bucket(const Key& key) const
  {
    return &m_buckets[Hash{}(key) % BUCKETS];
  }

  template <typename Key>
  static Node* Find(retro_atomic_ptr_t* bucket, const Key& key, const Node* stop)
  {
    for (auto* node = static_cast<Node*>(retro_atomic_load_acquire_ptr(bucket)); node != stop;
         node = node->next)
    {
      if (node->key == key)
        return node;
    }
    return nullptr;
  }

  static void FreeValue(quiesce_node_t* node) { delete reinterpret_cast<Value*>(node); }

  void Retire(void* value)
  {
    if (value)
      quiesce_retire(&m_quiesce, &static_cast<Value*>(value)->node, FreeValue);
  }

  void Replace(const K& key, Value* value)
  {
    quiesce_enter(&m_quiesce);
    retro_atomic_ptr_t* const bucket = Bucket(key);
    Node* node = Find(bucket, key, nullptr);
    if (!node)
    {
      auto* fresh = new Node{static_cast<Node*>(retro_atomic_load_acquire_ptr(bucket)), key, {}};
      retro_atomic_ptr_init(&fresh->value, nullptr);
      for (;;)
      {
        if (retro_atomic_cas_ptr(bucket, fresh->next, fresh))
        {
          node = fresh;
          break;
        }
        // Another thread pushed meanwhile, maybe this very key.
        Node* const head = static_cast<Node*>(retro_atomic_load_acquire_ptr(bucket));
        if ((node = Find(bucket, key, fresh->next)) != nullptr)
        {
          delete fresh;
          break;
        }
        fresh->next = head;
      }
    }
    Retire(retro_atomic_exchange_ptr(&node->value, value));
    quiesce_exit(&m_quiesce);
  }

  mutable retro_atomic_ptr_t m_buckets[BUCKETS]{};
  mutable quiesce_t m_quiesce{};
};

// The bare form: each key gets one atomic pointer slot, inserted on first use and never
// removed, and the caller decides what the pointer means and when what it points to may go.
template <typename K, size_t BUCKETS = 1024, typename Hash = std::hash<K>>
class AtomicSlotMap final
{
public:
  AtomicSlotMap() = default;
  AtomicSlotMap(const AtomicSlotMap&) = delete;
  AtomicSlotMap& operator=(const AtomicSlotMap&) = delete;

  ~AtomicSlotMap()
  {
    for (auto& bucket : m_buckets)
    {
      auto* node = static_cast<Node*>(retro_atomic_load_acquire_ptr(&bucket));
      while (node)
      {
        Node* const next = node->next;
        delete node;
        node = next;
      }
    }
  }

  // The slot for key, made empty (null) the first time.
  retro_atomic_ptr_t* Slot(const K& key)
  {
    retro_atomic_ptr_t* const bucket = &m_buckets[Hash{}(key) % BUCKETS];
    auto* head = static_cast<Node*>(retro_atomic_load_acquire_ptr(bucket));
    if (Node* node = Find(head, nullptr, key))
      return &node->slot;

    auto* fresh = new Node{head, key, {}};
    retro_atomic_ptr_init(&fresh->slot, nullptr);
    while (!retro_atomic_cas_ptr(bucket, fresh->next, fresh))
    {
      // Another thread pushed meanwhile, maybe this very key.
      auto* newer = static_cast<Node*>(retro_atomic_load_acquire_ptr(bucket));
      if (Node* node = Find(newer, fresh->next, key))
      {
        delete fresh;
        return &node->slot;
      }
      fresh->next = newer;
    }
    return &fresh->slot;
  }

  // Calls fn(slot) for every slot; for teardown, when no other thread uses the map.
  template <typename Fn>
  void ForEach(Fn&& fn)
  {
    for (auto& bucket : m_buckets)
    {
      for (auto* node = static_cast<Node*>(retro_atomic_load_acquire_ptr(&bucket)); node;
           node = node->next)
      {
        fn(&node->slot);
      }
    }
  }

private:
  struct Node
  {
    Node* next;  // fixed once published
    K key;
    retro_atomic_ptr_t slot;
  };

  static Node* Find(Node* node, const Node* stop, const K& key)
  {
    for (; node != stop; node = node->next)
    {
      if (node->key == key)
        return node;
    }
    return nullptr;
  }

  retro_atomic_ptr_t m_buckets[BUCKETS]{};
};
}  // namespace Common
