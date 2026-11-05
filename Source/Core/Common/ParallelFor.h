/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_COMMON_PARALLEL_FOR_H
#define DOLPHIN_COMMON_PARALLEL_FOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*parallel_for_fn)(void *ctx, size_t index);

/* Runs fn(ctx, i) for every i in [0, count) and returns once all of them
 * have run. The calls are spread over a pool of persistent worker threads
 * (one per extra core, started on first use) and the calling thread, which
 * takes jobs too. No locks: jobs are claimed with an atomic counter and the
 * workers park on an eventcount between batches.
 *
 * Calls may come from any thread. While one batch runs, a call from another
 * thread runs its jobs serially on that thread instead of waiting. A job must
 * not call parallel_for itself unless it accepts that serial fallback. */
void parallel_for(size_t count, parallel_for_fn fn, void *ctx);

/* Number of threads parallel_for may run jobs on, the caller included. */
unsigned parallel_for_threads(void);

/* Joins the worker threads. Call when no batch can be running (the core's
 * retro_deinit); the next parallel_for starts the pool again. */
void parallel_for_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
