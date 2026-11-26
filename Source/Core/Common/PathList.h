/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_PATHLIST_H
#define DOLPHIN_COMMON_PATHLIST_H

#include <stddef.h>

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A list of paths: their text packed, each terminated, in one buffer,
 * and their offsets in another, both grown by doubling and kept, so a
 * list that is cleared and filled again stops allocating. */
typedef struct path_list
{
  char* text;
  size_t text_size;
  size_t text_capacity;
  size_t* offsets;
  unsigned count;
  unsigned capacity;
} path_list_t;

void path_list_init(path_list_t* list);
/* Frees the storage; the list is empty and usable again. */
void path_list_free(path_list_t* list);
/* Empties the list, keeping its storage. */
void path_list_clear(path_list_t* list);
/* Adds a copy of path at the end; false when out of memory. */
bool path_list_add(path_list_t* list, const char* path);
/* Adds the first length bytes of text as a path; false when out of memory. */
bool path_list_add_len(path_list_t* list, const char* text, size_t length);
/* The path at index, valid until the list is next changed. */
const char* path_list_get(const path_list_t* list, unsigned index);
/* The index of the first path equal to path, or -1. */
int path_list_find(const path_list_t* list, const char* path);
/* Makes dst a copy of src, which may not be dst; false when out of memory. */
bool path_list_copy(path_list_t* dst, const path_list_t* src);
/* Moves src's paths into dst, freeing dst's; src is left empty. */
void path_list_move(path_list_t* dst, path_list_t* src);
/* Sorts the paths bytewise and drops repeats, as a set of strings keeps them. */
void path_list_sort_unique(path_list_t* list);

#ifdef __cplusplus
}
#endif

#endif
