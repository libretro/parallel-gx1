/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "VideoCommon/ShaderText.h"

#include "VideoCommon/ShaderArena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char s_empty[1] = {0};

void shader_text_init(struct shader_text *t, size_t cap)
{
   t->block  = NULL;
   t->len    = 0;
   t->cap    = 0;
   t->failed = 0;
   if (cap)
   {
      /* the block this thread's last text freed, where it is as large */
      size_t got = 0;
      t->block = shader_arena_text_take(SHADER_TEXT_HEAD_ROOM + cap, &got);
      if (t->block)
         cap = got - SHADER_TEXT_HEAD_ROOM;
      else
         t->block = (char*)malloc(SHADER_TEXT_HEAD_ROOM + cap);
      if (t->block)
      {
         t->cap = cap;
         t->block[SHADER_TEXT_HEAD_ROOM] = '\0';
      }
   }
}

void shader_text_free(struct shader_text *t)
{
   if (!t->block || !shader_arena_text_give(t->block, SHADER_TEXT_HEAD_ROOM + t->cap))
      free(t->block);
   t->block  = NULL;
   t->len    = 0;
   t->cap    = 0;
   t->failed = 0;
}

/* Room for n more bytes and the NUL; 0 once failed. */
static int shader_text_reserve(struct shader_text *t, size_t n)
{
   size_t need;
   size_t cap;
   char  *block;

   if (t->failed)
      return 0;
   need = t->len + n + 1;
   if (need <= t->cap)
      return 1;
   cap = t->cap ? t->cap : 1024;
   while (cap < need)
      cap *= 2;
   block = (char*)realloc(t->block, SHADER_TEXT_HEAD_ROOM + cap);
   if (!block)
   {
      t->failed = 1;
      return 0;
   }
   t->block = block;
   t->cap   = cap;
   return 1;
}

void shader_text_putn(struct shader_text *t, const char *s, size_t n)
{
   char *text;

   if (!shader_text_reserve(t, n))
      return;
   text = t->block + SHADER_TEXT_HEAD_ROOM;
   memcpy(text + t->len, s, n);
   t->len += n;
   text[t->len] = '\0';
}

void shader_text_puts(struct shader_text *t, const char *s)
{
   shader_text_putn(t, s, strlen(s));
}

void shader_text_indent(struct shader_text *t, unsigned n)
{
   char *text;

   if (!shader_text_reserve(t, n))
      return;
   text = t->block + SHADER_TEXT_HEAD_ROOM;
   memset(text + t->len, ' ', n);
   t->len += n;
   text[t->len] = '\0';
}

const char *shader_text_data(const struct shader_text *t)
{
   return t->block ? t->block + SHADER_TEXT_HEAD_ROOM : s_empty;
}

size_t shader_text_len(const struct shader_text *t)
{
   return t->len;
}

const char *shader_text_prepend(struct shader_text *t, const char *head, size_t head_len)
{
   char *start;

   if (head_len > SHADER_TEXT_HEAD_ROOM || !shader_text_reserve(t, 0))
      return NULL;
   start = t->block + SHADER_TEXT_HEAD_ROOM - head_len;
   memcpy(start, head, head_len);
   return start;
}
