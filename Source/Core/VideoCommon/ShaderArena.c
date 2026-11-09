/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>

#include "VideoCommon/ShaderArena.h"

#if defined(_MSC_VER)
#define SHADER_ARENA_THREAD_LOCAL __declspec(thread)
#else
#define SHADER_ARENA_THREAD_LOCAL __thread
#endif

/* the first block, and the largest a block grows to: rslc's own cap */
#define SHADER_ARENA_FIRST (1024u * 1024u)
#define SHADER_ARENA_MAX RSLC_ARENA_CAP_DEFAULT
/* threads that compile: the workers and the video thread */
#define SHADER_ARENA_THREADS 64

struct shader_arena
{
   void *block;
   size_t len;
};

/* every thread's block, to free them all; a slot is taken once per thread and generation */
static struct shader_arena s_arenas[SHADER_ARENA_THREADS];
static retro_atomic_int_t s_num_arenas;
/* bumped where every block is freed: a thread's slot from before is gone */
static retro_atomic_int_t s_generation;

static SHADER_ARENA_THREAD_LOCAL struct shader_arena *s_mine;
static SHADER_ARENA_THREAD_LOCAL int s_mine_generation;

static struct shader_arena *mine(void)
{
   const int generation = retro_atomic_load_acquire_int(&s_generation);
   int slot;
   if (s_mine && s_mine_generation == generation)
      return s_mine;
   s_mine = NULL;
   slot = retro_atomic_fetch_add_int(&s_num_arenas, 1);
   if (slot >= SHADER_ARENA_THREADS)
      return NULL;
   s_mine = &s_arenas[slot];
   s_mine_generation = generation;
   return s_mine;
}

void shader_arena_use(struct rslc_opts *opts)
{
   struct shader_arena *a = mine();
   if (!a)
      return;
   if (!a->block)
   {
      a->block = malloc(SHADER_ARENA_FIRST);
      a->len = a->block ? SHADER_ARENA_FIRST : 0;
   }
   opts->arena = a->block;
   opts->arena_len = a->len;
}

int shader_arena_grow(const char *err)
{
   struct shader_arena *a = mine();
   void *grown;
   /* rslc's words where the caller's block ran out */
   if (!a || !a->block || a->len >= SHADER_ARENA_MAX ||
       strncmp(err, "caller arena", 12) != 0)
      return 0;
   grown = malloc(a->len * 2);
   if (!grown)
      return 0;
   free(a->block);
   a->block = grown;
   a->len *= 2;
   return 1;
}

void shader_arenas_free(void)
{
   int i;
   const int n = retro_atomic_load_acquire_int(&s_num_arenas);
   for (i = 0; i < n && i < SHADER_ARENA_THREADS; i++)
   {
      free(s_arenas[i].block);
      s_arenas[i].block = NULL;
      s_arenas[i].len = 0;
   }
   retro_atomic_store_release_int(&s_num_arenas, 0);
   retro_atomic_fetch_add_int(&s_generation, 1);
}
