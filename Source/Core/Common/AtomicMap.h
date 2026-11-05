/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_ATOMICMAP_H
#define DOLPHIN_COMMON_ATOMICMAP_H

#include <stddef.h>

#include <retro_atomic.h>

#include "Common/Quiesce.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maps any thread may read and write without a lock, keyed by bytes.
 * A key is only ever added: its node is pushed at the head of its
 * bucket with a CAS and stays until the map is freed. */

/* The bare form: each key has one atomic pointer slot, null when made;
 * the caller decides what the pointer means and when what it points
 * to may go. */
typedef struct atomic_slot_map atomic_slot_map_t;

atomic_slot_map_t *atomic_slot_map_new(size_t buckets);
void atomic_slot_map_free(atomic_slot_map_t *map);

/* The key's slot, made the first time; NULL only when out of memory. */
retro_atomic_ptr_t *atomic_slot_map_slot(atomic_slot_map_t *map,
      const void *key, size_t len);
/* The key's slot if it has one, NULL otherwise. */
retro_atomic_ptr_t *atomic_slot_map_find(atomic_slot_map_t *map,
      const void *key, size_t len);
/* Calls fn on every slot; for teardown, while no other thread uses the map. */
void atomic_slot_map_for_each(atomic_slot_map_t *map,
      void (*fn)(retro_atomic_ptr_t *slot, void *ctx), void *ctx);

/* A map of values. A value starts with its quiesce_node_t; it is
 * published whole, and one replaced or erased is freed with free_value
 * once no reader is inside the map. */
typedef struct atomic_value_map atomic_value_map_t;

atomic_value_map_t *atomic_value_map_new(size_t buckets, quiesce_free_fn free_value);
/* Frees the values still held. */
void atomic_value_map_free(atomic_value_map_t *map);

/* Readers bracket atomic_value_map_get and every use of what it
 * returned with these. */
void atomic_value_map_enter(atomic_value_map_t *map);
void atomic_value_map_exit(atomic_value_map_t *map);
quiesce_node_t *atomic_value_map_get(atomic_value_map_t *map,
      const void *key, size_t len);

/* Takes value, which may be NULL to erase. */
void atomic_value_map_set(atomic_value_map_t *map,
      const void *key, size_t len, quiesce_node_t *value);

#ifdef __cplusplus
}
#endif

#endif
