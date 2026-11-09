/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * rslc's working memory, kept per thread between compiles. C89.
 *
 * Each thread that compiles (the compile workers, and the video thread
 * where it compiles itself) has one block, handed to rslc as its arena
 * (rslc_opts.arena): the parse, the IR and the emit allocate nothing
 * once the block has grown to the largest shader seen. Only the result
 * blob rslc returns is allocated, as the shader owns it.
 *
 *  - shader_arena_use fills opts->arena and opts->arena_len, and the
 *    allocator of the result blob (shader_arena_blob_alloc), making the
 *    block the first time; without one, rslc allocates as it would.
 *  - shader_arena_grow, after a compile that failed, doubles the block
 *    where the failure was the arena's, and says whether to compile
 *    again.
 *  - shader_arena_text_take and shader_arena_text_give keep a few freed
 *    shader text blocks per thread (ShaderText.c), the largest given, for
 *    the next texts that thread starts, as many as it has alive at once
 *    (a pipeline's vertex and pixel shaders): a generator allocates
 *    nothing once its thread has made as many, as large.
 *  - shader_arena_prepare makes a thread's block and text blocks before
 *    its first compile, where it starts at boot.
 *  - shader_arenas_free frees every thread's blocks, once no thread
 *    compiles any more (the backend shutting down); a thread's next
 *    compile makes new ones.
 */

#ifndef VIDEOCOMMON_SHADER_ARENA_H
#define VIDEOCOMMON_SHADER_ARENA_H

#include <gfx/shaders/rslc.h>

#ifdef __cplusplus
extern "C" {
#endif

void shader_arena_use(struct rslc_opts *opts);
int shader_arena_grow(const char *err);
/* this thread's kept text block, of at least size bytes, in *got its size: NULL for none */
char *shader_arena_text_take(size_t size, size_t *got);
/* a text block of size bytes kept for this thread: 0 where the caller frees it */
int shader_arena_text_give(char *block, size_t size);
/* rslc's allocator for its result blobs (shader_arena_use sets it), keeping freed
 * blobs per thread as shader text is kept; a blob is released with
 * shader_arena_blob_release, never free(). */
void *shader_arena_blob_alloc(void *ud, size_t len);
void shader_arena_blob_free(void *ud, void *ptr);
void shader_arena_blob_release(void *ptr);
void shader_arenas_free(void);
/* this thread's block made now, and texts text blocks of text_size bytes kept for it: a
 * thread about to compile (a worker starting) allocates nothing on its first compiles */
void shader_arena_prepare(size_t text_size, int texts);

#ifdef __cplusplus
}
#endif

#endif
