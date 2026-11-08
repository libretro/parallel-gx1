/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef VIDEOCOMMON_UID_TABLE_H
#define VIDEOCOMMON_UID_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a shader or pipeline uid maps to: the object built for it, which
 * the owner deletes, and whether it is still being compiled. */
struct uid_table_value
{
   void         *object;
   unsigned char pending;
};

/* An open-addressed table from fixed-size uids, compared byte for byte
 * as the uids are made to be, to a value each. A slot is the uid's hash
 * (0 when free), the uid, then its value. Grows only when more than half
 * full, which moves every value: a value found or added is good until the
 * next uid is added. */
struct uid_table
{
   unsigned char *slots;
   size_t         cap;       /* a power of two, or 0 */
   size_t         count;
   size_t         key_size;
   size_t         key_offset;
   size_t         value_offset;
   size_t         slot_size;
};

void uid_table_init(struct uid_table *t, size_t key_size);
void uid_table_free(struct uid_table *t);

/* The value of key, or NULL. */
struct uid_table_value *uid_table_find(const struct uid_table *t, const void *key);

/* The value of key, added zeroed if it was not there; NULL out of memory. */
struct uid_table_value *uid_table_get(struct uid_table *t, const void *key);

/* The next used slot from *cursor (start at 0), its uid in *key; NULL
 * after the last. Adding a uid while walking may move the slots. */
struct uid_table_value *uid_table_next(const struct uid_table *t, size_t *cursor,
      const void **key);

/* Forgets every uid; the owner has deleted their objects. */
void uid_table_clear(struct uid_table *t);

/* A growable array of fixed-size uids, kept in the order added. */
struct uid_array
{
   unsigned char *data;
   size_t         count;
   size_t         cap;
   size_t         elem_size;
};

void uid_array_init(struct uid_array *a, size_t elem_size);
void uid_array_free(struct uid_array *a);
/* Room for cap uids, so adding up to that many allocates nothing; 0 out of memory. */
int uid_array_reserve(struct uid_array *a, size_t cap);
/* Copies elem to the end; 0 out of memory. */
int uid_array_push(struct uid_array *a, const void *elem);
/* Moves the last uid into slot i, which goes. */
void uid_array_swap_remove(struct uid_array *a, size_t i);

#define uid_array_at(a, i) ((void*)((a)->data + (i) * (a)->elem_size))
#define uid_array_clear(a) ((a)->count = 0)

#ifdef __cplusplus
}
#endif

#endif
