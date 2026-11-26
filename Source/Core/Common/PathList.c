/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/PathList.h"

#include <stdlib.h>
#include <string.h>

void path_list_init(path_list_t *list)
{
  list->text = NULL;
  list->text_size = 0;
  list->text_capacity = 0;
  list->offsets = NULL;
  list->count = 0;
  list->capacity = 0;
}

void path_list_free(path_list_t *list)
{
  free(list->text);
  free(list->offsets);
  path_list_init(list);
}

void path_list_clear(path_list_t *list)
{
  list->text_size = 0;
  list->count = 0;
}

bool path_list_add_len(path_list_t *list, const char *text, size_t length)
{
  if (list->count == list->capacity)
  {
    unsigned capacity = list->capacity ? list->capacity * 2 : 8;
    size_t *offsets = (size_t *)realloc(list->offsets, capacity * sizeof(*offsets));
    if (!offsets)
      return false;
    list->offsets = offsets;
    list->capacity = capacity;
  }
  if (list->text_size + length + 1 > list->text_capacity)
  {
    size_t capacity = list->text_capacity ? list->text_capacity : 256;
    char *grown;
    while (capacity < list->text_size + length + 1)
      capacity *= 2;
    grown = (char *)realloc(list->text, capacity);
    if (!grown)
      return false;
    list->text = grown;
    list->text_capacity = capacity;
  }
  memcpy(list->text + list->text_size, text, length);
  list->text[list->text_size + length] = '\0';
  list->offsets[list->count++] = list->text_size;
  list->text_size += length + 1;
  return true;
}

bool path_list_add(path_list_t *list, const char *path)
{
  return path_list_add_len(list, path, strlen(path));
}

const char *path_list_get(const path_list_t *list, unsigned index)
{
  return list->text + list->offsets[index];
}

int path_list_find(const path_list_t *list, const char *path)
{
  unsigned i;
  for (i = 0; i < list->count; i++)
  {
    if (!strcmp(list->text + list->offsets[i], path))
      return (int)i;
  }
  return -1;
}

bool path_list_copy(path_list_t *dst, const path_list_t *src)
{
  unsigned i;
  path_list_clear(dst);
  for (i = 0; i < src->count; i++)
  {
    if (!path_list_add(dst, path_list_get(src, i)))
      return false;
  }
  return true;
}

void path_list_move(path_list_t *dst, path_list_t *src)
{
  path_list_free(dst);
  *dst = *src;
  path_list_init(src);
}
