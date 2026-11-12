// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* A map from a code address to a value of one type, in one block: open
 * addressing, linear probing. Entries are added and found, never taken
 * out one by one, only all at once by clear, which keeps the block: once
 * it has grown to what the code cache holds, adding allocates nothing.
 * Any address is a key, null included; ~0 marks a free slot.
 *
 *   CODE_PTR_MAP_DEFINE(trampolines, TrampolineInfo);
 *   trampolines_t m;
 *   trampolines_init(&m, 4096);
 *   TrampolineInfo* info = trampolines_get(&m, address);  (added, zeroed)
 *   TrampolineInfo* found = trampolines_find(&m, address); (NULL if not)
 *   trampolines_clear(&m);
 *   trampolines_free(&m);
 *
 * A pointer from get or find stays good until the next get adds.
 * The value is one C copies by assignment. Not thread safe. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CODE_PTR_MAP_FREE (~static_cast<uintptr_t>(0))

#define CODE_PTR_MAP_DEFINE(name, type)                                                            \
  typedef type name##_value_t;                                                                     \
  typedef struct name                                                                              \
  {                                                                                                \
    uintptr_t* keys;                                                                               \
    name##_value_t* values;                                                                        \
    size_t mask; /* slots less one, a power of two less one; 0 with none */                        \
    size_t count;                                                                                  \
  } name##_t;                                                                                      \
                                                                                                   \
  static inline size_t name##_slot(uintptr_t key, size_t mask)                                     \
  {                                                                                                \
    /* code addresses differ in their low bits: spread them over the table */                     \
    return static_cast<size_t>((static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> 32) & mask; \
  }                                                                                                \
                                                                                                   \
  static inline bool name##_alloc(name##_t* m, size_t slots)                                       \
  {                                                                                                \
    m->keys = static_cast<uintptr_t*>(malloc(slots * sizeof(uintptr_t)));                         \
    m->values = static_cast<name##_value_t*>(malloc(slots * sizeof(name##_value_t)));             \
    if (!m->keys || !m->values)                                                                    \
    {                                                                                              \
      free(m->keys);                                                                               \
      free(m->values);                                                                             \
      m->keys = nullptr;                                                                           \
      m->values = nullptr;                                                                         \
      m->mask = 0;                                                                                 \
      return false;                                                                                \
    }                                                                                              \
    memset(m->keys, 0xff, slots * sizeof(uintptr_t));                                              \
    m->mask = slots - 1;                                                                           \
    return true;                                                                                   \
  }                                                                                                \
                                                                                                   \
  /* Room for @expected entries before it grows */                                                \
  static inline bool name##_init(name##_t* m, size_t expected)                                     \
  {                                                                                                \
    size_t slots = 16;                                                                             \
    while (slots < expected * 2)                                                                   \
      slots <<= 1;                                                                                 \
    m->count = 0;                                                                                  \
    return name##_alloc(m, slots);                                                                 \
  }                                                                                                \
                                                                                                   \
  static inline void name##_free(name##_t* m)                                                      \
  {                                                                                                \
    free(m->keys);                                                                                 \
    free(m->values);                                                                               \
    m->keys = nullptr;                                                                             \
    m->values = nullptr;                                                                           \
    m->mask = 0;                                                                                   \
    m->count = 0;                                                                                  \
  }                                                                                                \
                                                                                                   \
  /* Every entry goes; the block stays */                                                          \
  static inline void name##_clear(name##_t* m)                                                     \
  {                                                                                                \
    if (m->keys && m->count)                                                                       \
      memset(m->keys, 0xff, (m->mask + 1) * sizeof(uintptr_t));                                    \
    m->count = 0;                                                                                  \
  }                                                                                                \
                                                                                                   \
  static inline name##_value_t* name##_find(const name##_t* m, const void* address)               \
  {                                                                                                \
    const uintptr_t key = reinterpret_cast<uintptr_t>(address);                                    \
    size_t i;                                                                                      \
    if (!m->keys)                                                                                  \
      return nullptr;                                                                              \
    for (i = name##_slot(key, m->mask);; i = (i + 1) & m->mask)                                    \
    {                                                                                              \
      if (m->keys[i] == key)                                                                       \
        return &m->values[i];                                                                      \
      if (m->keys[i] == CODE_PTR_MAP_FREE)                                                         \
        return nullptr;                                                                            \
    }                                                                                              \
  }                                                                                                \
                                                                                                   \
  /* Twice the slots, every entry moved: when it is half full */                                  \
  static inline bool name##_grow(name##_t* m)                                                      \
  {                                                                                                \
    name##_t larger;                                                                               \
    size_t i;                                                                                      \
    if (!name##_alloc(&larger, (m->mask + 1) * 2))                                                 \
      return false;                                                                                \
    for (i = 0; i <= m->mask; i++)                                                                 \
    {                                                                                              \
      size_t j;                                                                                    \
      if (m->keys[i] == CODE_PTR_MAP_FREE)                                                         \
        continue;                                                                                  \
      for (j = name##_slot(m->keys[i], larger.mask); larger.keys[j] != CODE_PTR_MAP_FREE;          \
           j = (j + 1) & larger.mask)                                                              \
      {                                                                                            \
      }                                                                                            \
      larger.keys[j] = m->keys[i];                                                                 \
      larger.values[j] = m->values[i];                                                             \
    }                                                                                              \
    larger.count = m->count;                                                                       \
    free(m->keys);                                                                                 \
    free(m->values);                                                                               \
    *m = larger;                                                                                   \
    return true;                                                                                   \
  }                                                                                                \
                                                                                                   \
  /* The value of @address, added zeroed if it was not there; NULL out of memory */               \
  static inline name##_value_t* name##_get(name##_t* m, const void* address)                      \
  {                                                                                                \
    const uintptr_t key = reinterpret_cast<uintptr_t>(address);                                    \
    size_t i;                                                                                      \
    if (!m->keys || (m->count + 1) * 2 > m->mask + 1)                                              \
    {                                                                                              \
      if (!(m->keys ? name##_grow(m) : name##_init(m, 16)))                                        \
        return nullptr;                                                                            \
    }                                                                                              \
    for (i = name##_slot(key, m->mask);; i = (i + 1) & m->mask)                                    \
    {                                                                                              \
      if (m->keys[i] == key)                                                                       \
        return &m->values[i];                                                                      \
      if (m->keys[i] == CODE_PTR_MAP_FREE)                                                         \
        break;                                                                                     \
    }                                                                                              \
    m->keys[i] = key;                                                                              \
    m->values[i] = name##_value_t{};                                                               \
    m->count++;                                                                                    \
    return &m->values[i];                                                                          \
  }                                                                                                \
  typedef name##_t name##_code_ptr_map_defined_t
