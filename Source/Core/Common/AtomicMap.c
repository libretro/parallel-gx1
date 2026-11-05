/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/AtomicMap.h"

#include <stdlib.h>
#include <string.h>

typedef struct atomic_map_node
{
   struct atomic_map_node *next; /* fixed once published */
   retro_atomic_ptr_t      slot;
   size_t                  len;
   unsigned char           key[1]; /* len bytes */
} atomic_map_node_t;

struct atomic_slot_map
{
   size_t             buckets;
   retro_atomic_ptr_t heads[1]; /* buckets entries */
};

struct atomic_value_map
{
   atomic_slot_map_t *slots;
   quiesce_free_fn    free_value;
   quiesce_t          quiesce;
};

/* FNV-1a */
static size_t atomic_map_hash(const void *key, size_t len)
{
   const unsigned char *bytes = (const unsigned char*)key;
   unsigned long        hash  = 2166136261UL;
   size_t               i;

   for (i = 0; i < len; i++)
   {
      hash ^= bytes[i];
      hash  = (hash * 16777619UL) & 0xffffffffUL;
   }
   return (size_t)hash;
}

static atomic_map_node_t *atomic_map_find(atomic_map_node_t *node,
      const atomic_map_node_t *stop, const void *key, size_t len)
{
   for (; node != stop; node = node->next)
   {
      if (node->len == len && memcmp(node->key, key, len) == 0)
         return node;
   }
   return NULL;
}

static retro_atomic_ptr_t *atomic_map_bucket(atomic_slot_map_t *map,
      const void *key, size_t len)
{
   return &map->heads[atomic_map_hash(key, len) % map->buckets];
}

atomic_slot_map_t *atomic_slot_map_new(size_t buckets)
{
   atomic_slot_map_t *map;
   size_t             i;

   if (buckets == 0)
      buckets = 1;
   map = (atomic_slot_map_t*)malloc(sizeof(*map)
         + (buckets - 1) * sizeof(map->heads[0]));
   if (!map)
      return NULL;
   map->buckets = buckets;
   for (i = 0; i < buckets; i++)
      retro_atomic_ptr_init(&map->heads[i], NULL);
   return map;
}

void atomic_slot_map_free(atomic_slot_map_t *map)
{
   size_t i;

   if (!map)
      return;
   for (i = 0; i < map->buckets; i++)
   {
      atomic_map_node_t *node =
         (atomic_map_node_t*)retro_atomic_load_acquire_ptr(&map->heads[i]);
      while (node)
      {
         atomic_map_node_t *next = node->next;
         free(node);
         node = next;
      }
   }
   free(map);
}

retro_atomic_ptr_t *atomic_slot_map_find(atomic_slot_map_t *map,
      const void *key, size_t len)
{
   atomic_map_node_t *node = atomic_map_find(
         (atomic_map_node_t*)retro_atomic_load_acquire_ptr(
            atomic_map_bucket(map, key, len)), NULL, key, len);
   return node ? &node->slot : NULL;
}

retro_atomic_ptr_t *atomic_slot_map_slot(atomic_slot_map_t *map,
      const void *key, size_t len)
{
   retro_atomic_ptr_t *bucket = atomic_map_bucket(map, key, len);
   atomic_map_node_t  *head   =
      (atomic_map_node_t*)retro_atomic_load_acquire_ptr(bucket);
   atomic_map_node_t  *node   = atomic_map_find(head, NULL, key, len);
   atomic_map_node_t  *fresh;

   if (node)
      return &node->slot;

   fresh = (atomic_map_node_t*)malloc(sizeof(*fresh) + (len ? len - 1 : 0));
   if (!fresh)
      return NULL;
   fresh->next = head;
   retro_atomic_ptr_init(&fresh->slot, NULL);
   fresh->len  = len;
   if (len)
      memcpy(fresh->key, key, len);

   while (!retro_atomic_cas_ptr(bucket, fresh->next, fresh))
   {
      /* Another thread pushed meanwhile, maybe this very key. */
      atomic_map_node_t *newer =
         (atomic_map_node_t*)retro_atomic_load_acquire_ptr(bucket);
      node = atomic_map_find(newer, fresh->next, key, len);
      if (node)
      {
         free(fresh);
         return &node->slot;
      }
      fresh->next = newer;
   }
   return &fresh->slot;
}

void atomic_slot_map_for_each(atomic_slot_map_t *map,
      void (*fn)(retro_atomic_ptr_t *slot, void *ctx), void *ctx)
{
   size_t i;

   for (i = 0; i < map->buckets; i++)
   {
      atomic_map_node_t *node =
         (atomic_map_node_t*)retro_atomic_load_acquire_ptr(&map->heads[i]);
      for (; node; node = node->next)
         fn(&node->slot, ctx);
   }
}

atomic_value_map_t *atomic_value_map_new(size_t buckets, quiesce_free_fn free_value)
{
   atomic_value_map_t *map = (atomic_value_map_t*)malloc(sizeof(*map));

   if (!map)
      return NULL;
   map->slots = atomic_slot_map_new(buckets);
   if (!map->slots)
   {
      free(map);
      return NULL;
   }
   map->free_value = free_value;
   quiesce_init(&map->quiesce);
   return map;
}

static void atomic_value_map_free_slot(retro_atomic_ptr_t *slot, void *ctx)
{
   atomic_value_map_t *map   = (atomic_value_map_t*)ctx;
   quiesce_node_t     *value = (quiesce_node_t*)retro_atomic_load_acquire_ptr(slot);

   if (value)
      map->free_value(value);
}

void atomic_value_map_free(atomic_value_map_t *map)
{
   if (!map)
      return;
   atomic_slot_map_for_each(map->slots, atomic_value_map_free_slot, map);
   quiesce_deinit(&map->quiesce);
   atomic_slot_map_free(map->slots);
   free(map);
}

void atomic_value_map_enter(atomic_value_map_t *map)
{
   quiesce_enter(&map->quiesce);
}

void atomic_value_map_exit(atomic_value_map_t *map)
{
   quiesce_exit(&map->quiesce);
}

quiesce_node_t *atomic_value_map_get(atomic_value_map_t *map,
      const void *key, size_t len)
{
   retro_atomic_ptr_t *slot = atomic_slot_map_find(map->slots, key, len);
   return slot ? (quiesce_node_t*)retro_atomic_load_acquire_ptr(slot) : NULL;
}

void atomic_value_map_set(atomic_value_map_t *map,
      const void *key, size_t len, quiesce_node_t *value)
{
   retro_atomic_ptr_t *slot;
   quiesce_node_t     *old = NULL;

   quiesce_enter(&map->quiesce);
   /* Erasing a key that was never added adds nothing. */
   slot = value ? atomic_slot_map_slot(map->slots, key, len)
                : atomic_slot_map_find(map->slots, key, len);
   if (slot)
      old = (quiesce_node_t*)retro_atomic_exchange_ptr(slot, value);
   else if (value)
      map->free_value(value);
   if (old)
      quiesce_retire(&map->quiesce, old, map->free_value);
   quiesce_exit(&map->quiesce);
}
