/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/Quiesce.h"

#include <stddef.h>

void quiesce_init(quiesce_t *q)
{
   retro_atomic_int_init(&q->readers, 0);
   mpsc_stack_init(&q->retired);
}

static void quiesce_free_chain(mpsc_stack_node_t *n)
{
   while (n)
   {
      mpsc_stack_node_t *next = n->next;
      quiesce_node_t    *qn   = (quiesce_node_t*)n;
      qn->free_fn(qn);
      n = next;
   }
}

void quiesce_deinit(quiesce_t *q)
{
   quiesce_free_chain(mpsc_stack_drain(&q->retired));
}

/* Take the retired chain first, then look for readers: everything taken
 * was unpublished before the take, so a reader still holding it was
 * counted before the look. */
static void quiesce_reclaim(quiesce_t *q)
{
   mpsc_stack_node_t *chain;
   if (mpsc_stack_empty(&q->retired))
      return;
   chain = mpsc_stack_drain(&q->retired);
   retro_atomic_thread_fence_seq_cst();
   if (!retro_atomic_load_seq_cst_int(&q->readers))
   {
      quiesce_free_chain(chain);
      return;
   }
   while (chain)
   {
      mpsc_stack_node_t *next = chain->next;
      mpsc_stack_push(&q->retired, chain);
      chain = next;
   }
}

void quiesce_enter(quiesce_t *q)
{
   retro_atomic_fetch_add_seq_cst_int(&q->readers, 1);
   retro_atomic_thread_fence_seq_cst();
}

void quiesce_exit(quiesce_t *q)
{
   if (retro_atomic_fetch_add_seq_cst_int(&q->readers, -1) == 1)
      quiesce_reclaim(q);
}

void quiesce_retire(quiesce_t *q, quiesce_node_t *node, quiesce_free_fn free_fn)
{
   node->free_fn = free_fn;
   mpsc_stack_push(&q->retired, &node->link);
   quiesce_reclaim(q);
}
