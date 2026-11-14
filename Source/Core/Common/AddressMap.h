// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* A map from a 32-bit address to a 32-bit value, in one block: open
 * addressing, linear probing, an entry taken out by moving the ones after
 * it back (no tombstones). The block grows only when it would be more than
 * half full, and clear keeps it, so once it is as large as the map gets,
 * nothing allocates. Any address is a key except ~0, which marks a free
 * slot (a page address never is).
 *
 *   address_map_t m;
 *   address_map_init(&m, 4096);              (rounded up to a power of two)
 *   u32* value = address_map_find(&m, key);  (NULL if not there)
 *   bool added;
 *   u32* slot = address_map_get(&m, key, &added);  (added zeroed if new)
 *   address_map_erase(&m, key);
 *   address_map_clear(&m);
 *   address_map_free(&m);
 *
 * A pointer from find or get stays good until the next get or erase.
 * Not thread safe. */

#include <stdlib.h>
#include <string.h>

#include "Common/CommonTypes.h"

#define ADDRESS_MAP_FREE 0xffffffffu

typedef struct address_map
{
  u32* keys;
  u32* values;
  u32 mask;   // slots - 1
  u32 shift;  // 32 - log2(slots)
  u32 count;
} address_map_t;

static inline u32 address_map_home(const address_map_t* m, u32 key)
{
  // page addresses are 4096 apart: their low bits say nothing
  return ((key >> 12) * 2654435761u + (key & 0xfffu)) >> m->shift;
}

static inline bool address_map_alloc(address_map_t* m, u32 slots)
{
  u32 bits = 1;
  while ((1u << bits) < slots)
    bits++;
  m->keys = static_cast<u32*>(malloc((static_cast<size_t>(1) << bits) * sizeof(u32)));
  m->values = static_cast<u32*>(malloc((static_cast<size_t>(1) << bits) * sizeof(u32)));
  if (!m->keys || !m->values)
  {
    free(m->keys);
    free(m->values);
    m->keys = nullptr;
    m->values = nullptr;
    m->mask = 0;
    m->shift = 32;
    m->count = 0;
    return false;
  }
  memset(m->keys, 0xff, (static_cast<size_t>(1) << bits) * sizeof(u32));
  m->mask = (1u << bits) - 1;
  m->shift = 32 - bits;
  m->count = 0;
  return true;
}

static inline bool address_map_init(address_map_t* m, u32 slots)
{
  return address_map_alloc(m, slots);
}

static inline void address_map_free(address_map_t* m)
{
  free(m->keys);
  free(m->values);
  m->keys = nullptr;
  m->values = nullptr;
  m->mask = 0;
  m->count = 0;
}

static inline void address_map_clear(address_map_t* m)
{
  if (m->keys && m->count)
    memset(m->keys, 0xff, (static_cast<size_t>(m->mask) + 1) * sizeof(u32));
  m->count = 0;
}

static inline u32* address_map_find(const address_map_t* m, u32 key)
{
  u32 i;
  if (!m->keys || key == ADDRESS_MAP_FREE)
    return nullptr;
  for (i = address_map_home(m, key);; i = (i + 1) & m->mask)
  {
    if (m->keys[i] == key)
      return &m->values[i];
    if (m->keys[i] == ADDRESS_MAP_FREE)
      return nullptr;
  }
}

static inline u32 address_map_slot_for(const address_map_t* m, u32 key)
{
  u32 i = address_map_home(m, key);
  while (m->keys[i] != ADDRESS_MAP_FREE && m->keys[i] != key)
    i = (i + 1) & m->mask;
  return i;
}

/* Twice the slots, every entry moved: when it would be more than half full */
static inline bool address_map_grow(address_map_t* m)
{
  address_map_t larger;
  u32 i;
  if (!address_map_alloc(&larger, m->keys ? (m->mask + 1) * 2 : 16))
    return false;
  if (m->keys)
  {
    for (i = 0; i <= m->mask; i++)
    {
      u32 j;
      if (m->keys[i] == ADDRESS_MAP_FREE)
        continue;
      j = address_map_slot_for(&larger, m->keys[i]);
      larger.keys[j] = m->keys[i];
      larger.values[j] = m->values[i];
    }
    larger.count = m->count;
  }
  free(m->keys);
  free(m->values);
  *m = larger;
  return true;
}

/* The value of key, added as 0 if it was not there (*added says which);
 * NULL out of memory or for the free marker */
static inline u32* address_map_get(address_map_t* m, u32 key, bool* added)
{
  u32 i;
  *added = false;
  if (key == ADDRESS_MAP_FREE)
    return nullptr;
  if (!m->keys || (m->count + 1) * 2 > m->mask + 1)
  {
    u32* const found = address_map_find(m, key);
    if (found)
      return found;
    if (!address_map_grow(m))
      return nullptr;
  }
  i = address_map_slot_for(m, key);
  if (m->keys[i] == ADDRESS_MAP_FREE)
  {
    m->keys[i] = key;
    m->values[i] = 0;
    m->count++;
    *added = true;
  }
  return &m->values[i];
}

static inline void address_map_erase(address_map_t* m, u32 key)
{
  u32 i, j;
  if (!m->count || key == ADDRESS_MAP_FREE)
    return;
  i = address_map_home(m, key);
  while (m->keys[i] != key)
  {
    if (m->keys[i] == ADDRESS_MAP_FREE)
      return;
    i = (i + 1) & m->mask;
  }
  // the ones after it, up to a free slot, move back into the gap unless their home is between
  // the gap and where they are
  j = i;
  for (;;)
  {
    u32 home;
    j = (j + 1) & m->mask;
    if (m->keys[j] == ADDRESS_MAP_FREE)
      break;
    home = address_map_home(m, m->keys[j]);
    if (i <= j ? (i < home && home <= j) : (i < home || home <= j))
      continue;
    m->keys[i] = m->keys[j];
    m->values[i] = m->values[j];
    i = j;
  }
  m->keys[i] = ADDRESS_MAP_FREE;
  m->count--;
}
