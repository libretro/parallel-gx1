/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "VideoCommon/UidTable.h"

#include <stdlib.h>
#include <string.h>

#include <encodings/xxh64.h>

#define UID_TABLE_INITIAL 256

static size_t uid_table_align8(size_t n)
{
   return (n + 7) & ~(size_t)7;
}

static uint64_t uid_table_hash(const struct uid_table *t, const void *key)
{
   /* never 0, the mark of a free slot */
   return xxh64(key, t->key_size, 0) | 1;
}

static uint64_t uid_table_slot_hash(const unsigned char *slot)
{
   uint64_t h;
   memcpy(&h, slot, sizeof(h));
   return h;
}

void uid_table_init(struct uid_table *t, size_t key_size)
{
   t->slots        = NULL;
   t->cap          = 0;
   t->count        = 0;
   t->key_size     = key_size;
   t->key_offset   = sizeof(uint64_t);
   t->value_offset = uid_table_align8(t->key_offset + key_size);
   t->slot_size    = uid_table_align8(t->value_offset + sizeof(struct uid_table_value));
}

void uid_table_free(struct uid_table *t)
{
   free(t->slots);
   t->slots = NULL;
   t->cap   = 0;
   t->count = 0;
}

/* The slot holding key, or the free one where it would go. */
static unsigned char *uid_table_probe(const struct uid_table *t, const void *key, uint64_t h)
{
   const size_t mask = t->cap - 1;
   size_t       i    = (size_t)h & mask;
   for (;;)
   {
      unsigned char *slot = t->slots + i * t->slot_size;
      const uint64_t sh   = uid_table_slot_hash(slot);
      if (!sh)
         return slot;
      if (sh == h && !memcmp(slot + t->key_offset, key, t->key_size))
         return slot;
      i = (i + 1) & mask;
   }
}

static int uid_table_grow(struct uid_table *t)
{
   const size_t   cap   = t->cap ? t->cap * 2 : UID_TABLE_INITIAL;
   unsigned char *slots = (unsigned char*)calloc(cap, t->slot_size);
   unsigned char *old   = t->slots;
   const size_t   old_cap = t->cap;
   size_t         i;

   if (!slots)
      return 0;
   t->slots = slots;
   t->cap   = cap;
   for (i = 0; i < old_cap; i++)
   {
      const unsigned char *slot = old + i * t->slot_size;
      const uint64_t       h    = uid_table_slot_hash(slot);
      if (h)
         memcpy(uid_table_probe(t, slot + t->key_offset, h), slot, t->slot_size);
   }
   free(old);
   return 1;
}

struct uid_table_value *uid_table_find(const struct uid_table *t, const void *key)
{
   unsigned char *slot;
   if (!t->count)
      return NULL;
   slot = uid_table_probe(t, key, uid_table_hash(t, key));
   if (!uid_table_slot_hash(slot))
      return NULL;
   return (struct uid_table_value*)(slot + t->value_offset);
}

struct uid_table_value *uid_table_get(struct uid_table *t, const void *key)
{
   const uint64_t h = uid_table_hash(t, key);
   unsigned char *slot;

   if (t->cap)
   {
      slot = uid_table_probe(t, key, h);
      if (uid_table_slot_hash(slot))
         return (struct uid_table_value*)(slot + t->value_offset);
   }
   if ((t->count + 1) * 2 > t->cap && !uid_table_grow(t))
      return NULL;
   slot = uid_table_probe(t, key, h);
   memcpy(slot, &h, sizeof(h));
   memcpy(slot + t->key_offset, key, t->key_size);
   memset(slot + t->value_offset, 0, sizeof(struct uid_table_value));
   t->count++;
   return (struct uid_table_value*)(slot + t->value_offset);
}

struct uid_table_value *uid_table_next(const struct uid_table *t, size_t *cursor,
      const void **key)
{
   while (*cursor < t->cap)
   {
      unsigned char *slot = t->slots + (*cursor)++ * t->slot_size;
      if (uid_table_slot_hash(slot))
      {
         *key = slot + t->key_offset;
         return (struct uid_table_value*)(slot + t->value_offset);
      }
   }
   return NULL;
}

void uid_table_clear(struct uid_table *t)
{
   if (t->slots)
      memset(t->slots, 0, t->cap * t->slot_size);
   t->count = 0;
}

void uid_array_init(struct uid_array *a, size_t elem_size)
{
   a->data      = NULL;
   a->count     = 0;
   a->cap       = 0;
   a->elem_size = elem_size;
}

void uid_array_free(struct uid_array *a)
{
   free(a->data);
   a->data  = NULL;
   a->count = 0;
   a->cap   = 0;
}

int uid_array_reserve(struct uid_array *a, size_t cap)
{
   unsigned char *data;
   if (cap <= a->cap)
      return 1;
   data = (unsigned char*)realloc(a->data, cap * a->elem_size);
   if (!data)
      return 0;
   a->data = data;
   a->cap  = cap;
   return 1;
}

int uid_array_push(struct uid_array *a, const void *elem)
{
   if (a->count == a->cap && !uid_array_reserve(a, a->cap ? a->cap * 2 : 16))
      return 0;
   memcpy(a->data + a->count * a->elem_size, elem, a->elem_size);
   a->count++;
   return 1;
}

void uid_array_swap_remove(struct uid_array *a, size_t i)
{
   a->count--;
   if (i != a->count)
      memcpy(a->data + i * a->elem_size, a->data + a->count * a->elem_size, a->elem_size);
}
