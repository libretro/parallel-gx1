/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/StringSet.h"

#include <stdlib.h>
#include <string.h>

void string_set_free(string_set_t *set)
{
   free(set->pool);
   free(set->offsets);
   memset(set, 0, sizeof(*set));
}

void string_set_clear(string_set_t *set)
{
   set->pool_size = 0;
   set->count     = 0;
}

/* The first slot whose string does not sort before the string of length
 * characters; *found says whether it is that string. */
static size_t string_set_find(const string_set_t *set, const char *string,
      size_t length, int *found)
{
   size_t low  = 0;
   size_t high = set->count;
   *found      = 0;
   while (low < high)
   {
      size_t mid        = low + (high - low) / 2;
      const char *entry = set->pool + set->offsets[mid];
      int result        = strncmp(entry, string, length);
      if (result == 0 && entry[length] != '\0')
         result = 1;
      if (result == 0)
      {
         *found = 1;
         return mid;
      }
      if (result < 0)
         low = mid + 1;
      else
         high = mid;
   }
   return low;
}

int string_set_add(string_set_t *set, const char *string, size_t length)
{
   int found;
   size_t slot = string_set_find(set, string, length, &found);
   if (found)
      return 0;

   if (set->pool_size + length + 1 > set->pool_capacity)
   {
      size_t capacity = set->pool_capacity ? set->pool_capacity : 1024;
      char *grown;
      while (capacity < set->pool_size + length + 1)
         capacity *= 2;
      grown = (char*)realloc(set->pool, capacity);
      if (!grown)
         return 0;
      set->pool          = grown;
      set->pool_capacity = capacity;
   }
   if (set->count == set->capacity)
   {
      size_t capacity = set->capacity ? set->capacity * 2 : 64;
      size_t *grown   = (size_t*)realloc(set->offsets, capacity * sizeof(size_t));
      if (!grown)
         return 0;
      set->offsets  = grown;
      set->capacity = capacity;
   }

   memcpy(set->pool + set->pool_size, string, length);
   set->pool[set->pool_size + length] = '\0';
   memmove(set->offsets + slot + 1, set->offsets + slot,
         (set->count - slot) * sizeof(size_t));
   set->offsets[slot] = set->pool_size;
   set->pool_size    += length + 1;
   set->count++;
   return 1;
}

int string_set_has(const string_set_t *set, const char *string, size_t length)
{
   int found;
   string_set_find(set, string, length, &found);
   return found;
}
