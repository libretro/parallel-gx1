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
/* blocks of one kind a thread has alive at once: a pipeline's stages */
#define SHADER_ARENA_KEPT 4

/* freed blocks kept for the next of their kind, with their sizes */
struct shader_stash
{
   char *block[SHADER_ARENA_KEPT];
   size_t len[SHADER_ARENA_KEPT];
};

struct shader_arena
{
   void *block;
   size_t len;
   struct shader_stash text; /* shader text (ShaderText.c) */
   struct shader_stash blob; /* rslc's results */
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
   /* the result blob, whatever the arena: it is freed with shader_arena_blob_free */
   opts->alloc = shader_arena_blob_alloc;
   opts->free_fn = shader_arena_blob_free;
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

/* the smallest kept block of at least size bytes: the larger stay for larger needs */
static char *stash_take(struct shader_stash *st, size_t size, size_t *got)
{
   char *block;
   int i, best = -1;
   for (i = 0; i < SHADER_ARENA_KEPT; i++)
      if (st->block[i] && st->len[i] >= size && (best < 0 || st->len[i] < st->len[best]))
         best = i;
   if (best < 0)
      return NULL;
   block = st->block[best];
   *got = st->len[best];
   st->block[best] = NULL;
   st->len[best] = 0;
   return block;
}

/* kept in an empty place, or else in place of the smallest kept, where smaller */
static int stash_give(struct shader_stash *st, char *block, size_t size)
{
   int i, smallest = 0;
   for (i = 0; i < SHADER_ARENA_KEPT; i++)
   {
      if (!st->block[i])
      {
         smallest = i;
         break;
      }
      if (st->len[i] < st->len[smallest])
         smallest = i;
   }
   if (st->block[smallest] && st->len[smallest] >= size)
      return 0;
   free(st->block[smallest]);
   st->block[smallest] = block;
   st->len[smallest] = size;
   return 1;
}

static void stash_free(struct shader_stash *st)
{
   int i;
   for (i = 0; i < SHADER_ARENA_KEPT; i++)
   {
      free(st->block[i]);
      st->block[i] = NULL;
      st->len[i] = 0;
   }
}

char *shader_arena_text_take(size_t size, size_t *got)
{
   struct shader_arena *a = mine();
   return a ? stash_take(&a->text, size, got) : NULL;
}

int shader_arena_text_give(char *block, size_t size)
{
   struct shader_arena *a = mine();
   return a ? stash_give(&a->text, block, size) : 0;
}

/* A blob's size goes in front of it, for the free that is given only the blob; the
 * room keeps the blob as aligned as malloc's. */
#define SHADER_BLOB_HEAD 16

void *shader_arena_blob_alloc(void *ud, size_t len)
{
   struct shader_arena *a = mine();
   size_t got = 0;
   char *block = a ? stash_take(&a->blob, SHADER_BLOB_HEAD + len, &got) : NULL;
   (void)ud;
   if (!block)
   {
      got = SHADER_BLOB_HEAD + len;
      block = (char *)malloc(got);
      if (!block)
         return NULL;
   }
   memcpy(block, &got, sizeof(got));
   return block + SHADER_BLOB_HEAD;
}

void shader_arena_blob_free(void *ud, void *ptr)
{
   struct shader_arena *a;
   char *block;
   size_t size;
   (void)ud;
   if (!ptr)
      return;
   block = (char *)ptr - SHADER_BLOB_HEAD;
   memcpy(&size, block, sizeof(size));
   a = mine();
   if (!a || !stash_give(&a->blob, block, size))
      free(block);
}

void shader_arena_blob_release(void *ptr)
{
   shader_arena_blob_free(NULL, ptr);
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
      stash_free(&s_arenas[i].text);
      stash_free(&s_arenas[i].blob);
   }
   retro_atomic_store_release_int(&s_num_arenas, 0);
   retro_atomic_fetch_add_int(&s_generation, 1);
}
