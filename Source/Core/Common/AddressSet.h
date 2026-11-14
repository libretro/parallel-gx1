// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* A set of 32-bit addresses other than 0, in one block: open addressing,
 * linear probing, an address taken out by moving the ones after it back
 * (no tombstones). The block is made by init and grows only when it would
 * be more than half full, so once it is as large as the set gets, adding
 * and taking out allocate nothing. 0 marks a free slot.
 *
 *   address_set_t s;
 *   address_set_init(&s, 1024);       (a power of two)
 *   address_set_insert(&s, address);
 *   if (address_set_contains(&s, address)) ...
 *   address_set_erase(&s, address);
 *   address_set_clear(&s);            (keeps the block)
 *   address_set_free(&s);
 *
 * Not thread safe. */

#include <stdlib.h>
#include <string.h>

#include "Common/CommonTypes.h"

typedef struct address_set
{
  u32* slots;
  u32 mask;   // slots - 1
  u32 shift;  // 32 - log2(slots)
  u32 count;
} address_set_t;

static inline u32 address_set_home(const address_set_t* s, u32 address)
{
  // instructions are 4 apart: their low bits say nothing
  return ((address >> 2) * 2654435761u) >> s->shift;
}

static inline void address_set_alloc(address_set_t* s, u32 capacity)
{
  u32 bits = 0;
  while ((1u << bits) < capacity)
    bits++;
  s->slots = static_cast<u32*>(calloc(static_cast<size_t>(1) << bits, sizeof(u32)));
  s->mask = (1u << bits) - 1;
  s->shift = 32 - bits;
  s->count = 0;
}

static inline void address_set_init(address_set_t* s, u32 capacity)
{
  address_set_alloc(s, capacity < 2 ? 2 : capacity);
}

static inline void address_set_free(address_set_t* s)
{
  free(s->slots);
  s->slots = nullptr;
  s->mask = 0;
  s->count = 0;
}

static inline void address_set_clear(address_set_t* s)
{
  if (s->count)
    memset(s->slots, 0, (static_cast<size_t>(s->mask) + 1) * sizeof(u32));
  s->count = 0;
}

static inline bool address_set_contains(const address_set_t* s, u32 address)
{
  u32 i = address_set_home(s, address);
  for (;;)
  {
    const u32 slot = s->slots[i];
    if (slot == address)
      return address != 0;
    if (slot == 0)
      return false;
    i = (i + 1) & s->mask;
  }
}

static inline void address_set_put(address_set_t* s, u32 address)
{
  u32 i = address_set_home(s, address);
  while (s->slots[i] != 0)
  {
    if (s->slots[i] == address)
      return;
    i = (i + 1) & s->mask;
  }
  s->slots[i] = address;
  s->count++;
}

static inline void address_set_insert(address_set_t* s, u32 address)
{
  if (address == 0)
    return;
  if ((s->count + 1) * 2 > s->mask + 1)
  {
    // more than half full: twice the block, everything put again
    const address_set_t old = *s;
    u32 i;
    address_set_alloc(s, (old.mask + 1) * 2);
    for (i = 0; i <= old.mask; i++)
    {
      if (old.slots[i])
        address_set_put(s, old.slots[i]);
    }
    free(old.slots);
  }
  address_set_put(s, address);
}

static inline void address_set_erase(address_set_t* s, u32 address)
{
  u32 i, j;
  if (address == 0 || s->count == 0)
    return;
  i = address_set_home(s, address);
  while (s->slots[i] != address)
  {
    if (s->slots[i] == 0)
      return;
    i = (i + 1) & s->mask;
  }
  // the ones after it, up to a free slot, move back into the gap unless their home is between
  // the gap and where they are
  j = i;
  for (;;)
  {
    u32 home;
    j = (j + 1) & s->mask;
    if (s->slots[j] == 0)
      break;
    home = address_set_home(s, s->slots[j]);
    if (i <= j ? (i < home && home <= j) : (i < home || home <= j))
      continue;
    s->slots[i] = s->slots[j];
    i = j;
  }
  s->slots[i] = 0;
  s->count--;
}
