/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_QUIESCE_H
#define DOLPHIN_COMMON_QUIESCE_H

#include <boolean.h>
#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deferred freeing for read-mostly shared data, without locks.
 *
 * Readers bracket every access with quiesce_enter / quiesce_exit.
 * A writer publishes a new version with an atomic pointer store or
 * exchange and hands the old one to quiesce_retire; it is freed once
 * no reader is inside, by whichever thread sees the count reach zero.
 * Nobody waits: a writer never blocks on readers, and a reader that
 * retires something (a callback removing itself) only defers the free.
 *
 * The publication and the reader count are ordered with seq_cst
 * operations, so a reader either is counted when a retirement checks,
 * or sees the new pointer. */

struct quiesce_node;
typedef void (*quiesce_free_fn)(struct quiesce_node *node);

/* Embedded in whatever is retired, so retiring never allocates. */
typedef struct quiesce_node
{
   mpsc_stack_node_t link;
   quiesce_free_fn   free_fn;
} quiesce_node_t;

typedef struct quiesce
{
   retro_atomic_int_t readers;
   mpsc_stack_t       retired;
} quiesce_t;

void quiesce_init(quiesce_t *q);
/* Frees everything still retired; no reader may be inside. */
void quiesce_deinit(quiesce_t *q);

void quiesce_enter(quiesce_t *q);
void quiesce_exit(quiesce_t *q);

/* node's object was unpublished before this call; free_fn(node) runs
 * once no reader that could have seen it is inside. */
void quiesce_retire(quiesce_t *q, quiesce_node_t *node, quiesce_free_fn free_fn);

#ifdef __cplusplus
}
#endif

#endif
