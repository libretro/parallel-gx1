/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/ParallelFor.h"

#include <stdlib.h>

#include <features/features_cpu.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

/* Never more workers than this, whatever the core count. */
#define PARALLEL_FOR_MAX_WORKERS 63

/* One batch at a time, owned by whichever caller won m_owner.
 *
 * A worker touches the batch only between raising m_users and lowering it
 * again, and only if it saw m_active set in between. The caller clears
 * m_active once every index is claimed and then waits for m_users to drop
 * to zero; both sides use seq_cst operations, so a worker either is counted
 * in m_users or sees m_active clear. Once m_users reads zero no worker can
 * still be inside the batch, and the caller is free to return and to rewrite
 * the batch fields for the next call. */
static struct
{
   sthread_t **workers;
   unsigned worker_count;
   int started;

   /* Batch, written only by the owner while no worker is inside it. */
   parallel_for_fn fn;
   void *ctx;
   size_t count;
   retro_atomic_size_t next;

   retro_atomic_int_t owner;
   retro_atomic_int_t active;
   retro_atomic_int_t users;
   retro_atomic_int_t generation;
   retro_atomic_int_t stop;

   retro_eventcount_t wake;
   retro_eventcount_t idle;
} s_pool;

static void parallel_for_run_jobs(void)
{
   for (;;)
   {
      const size_t i = retro_atomic_fetch_add_size(&s_pool.next, 1);
      if (i >= s_pool.count)
         return;
      s_pool.fn(s_pool.ctx, i);
   }
}

static void parallel_for_worker(void *data)
{
   int seen = 0;
   (void)data;

   sthread_setname("parallel_for");

   for (;;)
   {
      int gen;
      const int key = retro_eventcount_prepare_wait(&s_pool.wake);

      if (retro_atomic_load_acquire_int(&s_pool.stop))
      {
         retro_eventcount_cancel_wait(&s_pool.wake);
         return;
      }

      gen = retro_atomic_load_acquire_int(&s_pool.generation);
      if (gen == seen)
      {
         retro_eventcount_commit_wait(&s_pool.wake, key);
         continue;
      }
      retro_eventcount_cancel_wait(&s_pool.wake);
      seen = gen;

      retro_atomic_fetch_add_seq_cst_int(&s_pool.users, 1);
      if (retro_atomic_load_seq_cst_int(&s_pool.active))
         parallel_for_run_jobs();
      if (retro_atomic_fetch_add_seq_cst_int(&s_pool.users, -1) == 1)
         retro_eventcount_notify(&s_pool.idle);
   }
}

/* Called by the owner. */
static void parallel_for_start(void)
{
   unsigned cores = cpu_features_get_core_amount();
   unsigned i;

   s_pool.started = 1;
   if (cores <= 1)
      return;
   if (cores - 1 > PARALLEL_FOR_MAX_WORKERS)
      cores = PARALLEL_FOR_MAX_WORKERS + 1;

   s_pool.workers = (sthread_t**)calloc(cores - 1, sizeof(*s_pool.workers));
   if (!s_pool.workers)
      return;

   retro_atomic_int_init(&s_pool.active, 0);
   retro_atomic_int_init(&s_pool.users, 0);
   retro_atomic_int_init(&s_pool.generation, 0);
   retro_atomic_int_init(&s_pool.stop, 0);
   retro_atomic_size_init(&s_pool.next, 0);
   if (!retro_eventcount_init(&s_pool.wake))
      goto fail_wake;
   if (!retro_eventcount_init(&s_pool.idle))
      goto fail_idle;

   for (i = 0; i < cores - 1; i++)
   {
      s_pool.workers[i] = sthread_create(parallel_for_worker, NULL);
      if (!s_pool.workers[i])
         break;
   }
   s_pool.worker_count = i;
   if (i)
      return;

   retro_eventcount_free(&s_pool.idle);
fail_idle:
   retro_eventcount_free(&s_pool.wake);
fail_wake:
   free(s_pool.workers);
   s_pool.workers = NULL;
}

static void parallel_for_serial(size_t count, parallel_for_fn fn, void *ctx)
{
   size_t i;
   for (i = 0; i < count; i++)
      fn(ctx, i);
}

void parallel_for(size_t count, parallel_for_fn fn, void *ctx)
{
   if (count < 2 || !retro_atomic_cas_int(&s_pool.owner, 0, 1))
   {
      parallel_for_serial(count, fn, ctx);
      return;
   }

   if (!s_pool.started)
      parallel_for_start();

   if (!s_pool.worker_count)
   {
      parallel_for_serial(count, fn, ctx);
      retro_atomic_store_release_int(&s_pool.owner, 0);
      return;
   }

   s_pool.fn    = fn;
   s_pool.ctx   = ctx;
   s_pool.count = count;
   retro_atomic_store_relaxed_size(&s_pool.next, 0);

   retro_atomic_fetch_add_seq_cst_int(&s_pool.active, 1);
   retro_atomic_fetch_add_int(&s_pool.generation, 1);
   retro_eventcount_notify(&s_pool.wake);

   parallel_for_run_jobs();

   /* Every index is claimed; wait for the workers still running one. */
   retro_atomic_fetch_add_seq_cst_int(&s_pool.active, -1);
   while (retro_atomic_load_seq_cst_int(&s_pool.users))
   {
      const int key = retro_eventcount_prepare_wait(&s_pool.idle);
      if (!retro_atomic_load_seq_cst_int(&s_pool.users))
      {
         retro_eventcount_cancel_wait(&s_pool.idle);
         break;
      }
      retro_eventcount_commit_wait(&s_pool.idle, key);
   }

   retro_atomic_store_release_int(&s_pool.owner, 0);
}

unsigned parallel_for_threads(void)
{
   unsigned cores = cpu_features_get_core_amount();
   if (cores < 1)
      return 1;
   if (cores > PARALLEL_FOR_MAX_WORKERS + 1)
      return PARALLEL_FOR_MAX_WORKERS + 1;
   return cores;
}

void parallel_for_deinit(void)
{
   unsigned i;

   while (!retro_atomic_cas_int(&s_pool.owner, 0, 1))
      sthread_yield();

   if (s_pool.worker_count)
   {
      retro_atomic_store_release_int(&s_pool.stop, 1);
      retro_eventcount_notify(&s_pool.wake);
      for (i = 0; i < s_pool.worker_count; i++)
         sthread_join(s_pool.workers[i]);
      retro_eventcount_free(&s_pool.idle);
      retro_eventcount_free(&s_pool.wake);
   }
   free(s_pool.workers);
   s_pool.workers      = NULL;
   s_pool.worker_count = 0;
   s_pool.started      = 0;

   retro_atomic_store_release_int(&s_pool.owner, 0);
}
