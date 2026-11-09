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
 *  - shader_arena_use fills opts->arena and opts->arena_len, making the
 *    block the first time; without one, rslc allocates as it would.
 *  - shader_arena_grow, after a compile that failed, doubles the block
 *    where the failure was the arena's, and says whether to compile
 *    again.
 *  - shader_arenas_free frees every thread's block, once no thread
 *    compiles any more (the backend shutting down); a thread's next
 *    compile makes a new one.
 */

#ifndef VIDEOCOMMON_SHADER_ARENA_H
#define VIDEOCOMMON_SHADER_ARENA_H

#include <gfx/shaders/rslc.h>

#ifdef __cplusplus
extern "C" {
#endif

void shader_arena_use(struct rslc_opts *opts);
int shader_arena_grow(const char *err);
void shader_arenas_free(void);

#ifdef __cplusplus
}
#endif

#endif
