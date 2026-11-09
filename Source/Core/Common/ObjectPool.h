/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_OBJECTPOOL_H
#define DOLPHIN_COMMON_OBJECTPOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Objects of one size, from chunks made a few at a time, for a class's
 * operator new and delete: any thread allocates and frees, without
 * locks, and after the chunks are made nothing allocates.
 *
 * A freed object is pushed on a stack; an allocation takes the stack
 * whole (an exchange, so no ABA), keeps one and pushes the rest back,
 * or else takes the next slot of the chunks, making a chunk where the
 * slot is the first of one. Past the last chunk, objects come from
 * malloc; each object's header says which, for the free. */

typedef struct object_pool object_pool_t;

/* A pool for objects of at most object_size bytes, its first chunk of
 * chunk_slots made now (at boot, say); NULL when out of memory. */
object_pool_t *object_pool_create(size_t object_size, int chunk_slots);
/* Frees the pool and its chunks: now where none of its objects is out,
 * or else with the free of the last. No new object comes from it. */
void object_pool_destroy(object_pool_t *pool);
/* An object of size bytes, from the pool where it is at most the pool's
 * object size (pool may be NULL: from malloc); NULL when out of memory. */
void *object_pool_alloc(object_pool_t *pool, size_t size);
/* Frees an object object_pool_alloc returned, whichever made it. */
void object_pool_free(void *object);

#ifdef __cplusplus
}

/* A class whose objects come from its own pool: OBJECT_POOL_MEMBERS in it,
 * OBJECT_POOL_DEFINE(Class, chunk slots) in its source; CreatePool before
 * the first object, DestroyPool once the last is gone. */
#define OBJECT_POOL_MEMBERS                                                                        \
  static void* operator new(size_t size);                                                    \
  static void operator delete(void* ptr);                                                         \
  static void CreatePool();                                                                        \
  static void DestroyPool();

#define OBJECT_POOL_DEFINE(C, slots)                                                               \
  static object_pool_t* s_##C##_pool;                                                             \
  void* C::operator new(size_t size) { return object_pool_alloc(s_##C##_pool, size); }      \
  void C::operator delete(void* ptr) { object_pool_free(ptr); }                                   \
  void C::CreatePool()                                                                             \
  {                                                                                                \
    if (!s_##C##_pool)                                                                            \
      s_##C##_pool = object_pool_create(sizeof(C), (slots));                                      \
  }                                                                                                \
  void C::DestroyPool()                                                                            \
  {                                                                                                \
    object_pool_destroy(s_##C##_pool);                                                            \
    s_##C##_pool = nullptr;                                                                       \
  }
#endif

#endif
