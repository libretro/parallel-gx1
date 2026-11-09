/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdlib.h>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

#include "Common/ObjectPool.h"

#define OBJECT_POOL_CHUNKS 64

struct object_pool
{
   size_t object_size; /* the largest object it holds */
   size_t slot_size;   /* with its header, rounded */
   int chunk_slots;
   retro_atomic_int_t next; /* slots handed out from the chunks */
   retro_atomic_ptr_t chunks[OBJECT_POOL_CHUNKS];
   mpsc_stack_t freed;
   retro_atomic_int_t live;  /* objects out of the chunks */
   retro_atomic_int_t dying; /* 1 once destroyed, 2 once freed */
};

/* In front of every object: its pool, NULL where malloc made it alone,
 * and the stack node it is while freed. 16 bytes keep the object as
 * aligned as malloc's. */
struct object_head
{
   object_pool_t *pool;
   mpsc_stack_node_t node;
};

#define OBJECT_HEAD 16

typedef char object_head_fits[sizeof(struct object_head) <= OBJECT_HEAD ? 1 : -1];

static void *object_of(struct object_head *head)
{
   return (char *)head + OBJECT_HEAD;
}

#if defined(RETRO_ATOMIC_HAS_PTR)
/* Chunk n, made where it is not yet: whichever thread publishes first
 * wins, the others free theirs. */
static char *chunk(object_pool_t *pool, int n)
{
   char *have = (char *)retro_atomic_load_acquire_ptr(&pool->chunks[n]);
   char *made;
   if (have)
      return have;
   made = (char *)malloc(pool->slot_size * (size_t)pool->chunk_slots);
   if (!made)
      return NULL;
   if (retro_atomic_cas_ptr(&pool->chunks[n], NULL, made))
      return made;
   free(made);
   return (char *)retro_atomic_load_acquire_ptr(&pool->chunks[n]);
}
#endif

object_pool_t *object_pool_create(size_t object_size, int chunk_slots)
{
   object_pool_t *pool = (object_pool_t *)malloc(sizeof(*pool));
   int i;
   if (!pool)
      return NULL;
   pool->object_size = object_size;
   pool->slot_size = (OBJECT_HEAD + object_size + 15) & ~(size_t)15;
   pool->chunk_slots = chunk_slots > 0 ? chunk_slots : 1;
   retro_atomic_int_init(&pool->next, 0);
   for (i = 0; i < OBJECT_POOL_CHUNKS; i++)
      retro_atomic_ptr_init(&pool->chunks[i], NULL);
   mpsc_stack_init(&pool->freed);
   retro_atomic_int_init(&pool->live, 0);
   retro_atomic_int_init(&pool->dying, 0);
#if defined(RETRO_ATOMIC_HAS_PTR)
   chunk(pool, 0); /* where it cannot be made now, the first object tries again */
#endif
   return pool;
}

static void free_pool(object_pool_t *pool)
{
   int i;
   for (i = 0; i < OBJECT_POOL_CHUNKS; i++)
      free(retro_atomic_load_acquire_ptr(&pool->chunks[i]));
   free(pool);
}

/* Freed now where no object is out, or else by the free of the last: whichever
 * sees both moves dying from 1 to 2 and frees it. */
void object_pool_destroy(object_pool_t *pool)
{
   int expected = 1;
   if (!pool)
      return;
   retro_atomic_fetch_add_seq_cst_int(&pool->dying, 1); /* 0 to 1 */
   if (retro_atomic_load_seq_cst_int(&pool->live) == 0 &&
       retro_atomic_cas_int(&pool->dying, expected, 2))
      free_pool(pool);
}

void *object_pool_alloc(object_pool_t *pool, size_t size)
{
   struct object_head *head;
#if defined(RETRO_ATOMIC_HAS_PTR)
   if (pool && size <= pool->object_size)
   {
      int slot;
      /* a freed one: the stack is taken whole, the rest goes back */
      mpsc_stack_node_t *taken = mpsc_stack_drain(&pool->freed);
      if (taken)
      {
         mpsc_stack_node_t *rest = taken->next;
         if (rest)
         {
            /* the chain is this thread's: pushed whole, its last linked to the head */
            mpsc_stack_node_t *last = rest;
            while (last->next)
               last = last->next;
            do
            {
               last->next =
                     (mpsc_stack_node_t *)retro_atomic_load_acquire_ptr(&pool->freed.head);
            } while (!retro_atomic_cas_ptr(&pool->freed.head, last->next, rest));
         }
         head = (struct object_head *)((char *)taken - offsetof(struct object_head, node));
         retro_atomic_fetch_add_seq_cst_int(&pool->live, 1);
         return object_of(head);
      }

      /* the next slot of the chunks */
      slot = retro_atomic_fetch_add_int(&pool->next, 1);
      if (slot >= 0 && slot / pool->chunk_slots < OBJECT_POOL_CHUNKS)
      {
         char *c = chunk(pool, slot / pool->chunk_slots);
         if (c)
         {
            head = (struct object_head *)(c + pool->slot_size *
                                                  (size_t)(slot % pool->chunk_slots));
            head->pool = pool;
            retro_atomic_fetch_add_seq_cst_int(&pool->live, 1);
            return object_of(head);
         }
      }
   }
#endif
   /* past the chunks, too large, or without pointer atomics */
   head = (struct object_head *)malloc(OBJECT_HEAD + size);
   if (!head)
      return NULL;
   head->pool = NULL;
   return object_of(head);
}

void object_pool_free(void *object)
{
   struct object_head *head;
   if (!object)
      return;
   head = (struct object_head *)((char *)object - OBJECT_HEAD);
   if (!head->pool)
   {
      free(head);
      return;
   }
   {
      object_pool_t *pool = head->pool;
      int expected = 1;
      mpsc_stack_push(&pool->freed, &head->node);
      if (retro_atomic_fetch_add_seq_cst_int(&pool->live, -1) == 1 &&
          retro_atomic_load_seq_cst_int(&pool->dying) == 1 &&
          retro_atomic_cas_int(&pool->dying, expected, 2))
         free_pool(pool);
   }
}
