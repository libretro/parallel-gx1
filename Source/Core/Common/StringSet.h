/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_STRINGSET_H
#define DOLPHIN_COMMON_STRINGSET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A set of strings: their characters NUL-terminated one after another in
 * one pool, and their offsets in another block kept sorted by string, so
 * a lookup is a binary search and nothing is allocated per string. A
 * string may hold any byte but NUL. Zero-filled is empty. */
typedef struct string_set
{
   char *pool;
   size_t pool_size;
   size_t pool_capacity;
   size_t *offsets;
   size_t count;
   size_t capacity;
} string_set_t;

/* Frees the set's blocks; it is then empty. */
void string_set_free(string_set_t *set);
/* Empties the set, keeping its blocks. */
void string_set_clear(string_set_t *set);
/* Puts the string of length characters in; 1 when it was put in, 0 when
 * it was there already or memory ran out. */
int string_set_add(string_set_t *set, const char *string, size_t length);
/* Whether the set has the string of length characters. */
int string_set_has(const string_set_t *set, const char *string, size_t length);

#ifdef __cplusplus
}
#endif

#endif
