/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef VIDEOCOMMON_STUTTER_STATS_H
#define VIDEOCOMMON_STUTTER_STATS_H

/* What the stutter harness (Tools/stutter) counts: each kind of shader
 * and pipeline work, how many times it happened and the CPU cycles it
 * took, by where it ran. Only the stats build (make STATS=1) has them;
 * in every other build each macro is nothing. The harness reads them
 * through parallel_gx1_stutter_stats. */

#ifdef HAVE_STUTTER_STATS
#include <features/features_cpu.h>
#include <libretro.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The order is the harness's: new kinds go at the end. */
enum stutter_stat
{
   /* built on the spot for a draw, which waited for it */
   STUTTER_DRAW_GX_PIPELINE = 0,
   STUTTER_DRAW_UBER_PIPELINE,
   STUTTER_DRAW_EFB_COPY_TO_VRAM,
   STUTTER_DRAW_EFB_COPY_TO_RAM,
   STUTTER_DRAW_REINTERPRET,
   STUTTER_DRAW_DECODE,
   /* a queued item compiled by the thread that queued it: no workers, no
    * room in the queue, or a deferred compile on the video thread */
   STUTTER_DRAW_QUEUED_COMPILE,
   /* the video thread waiting for the compile workers */
   STUTTER_DRAW_WAIT,
   /* a cache file written by the thread building for a draw */
   STUTTER_DRAW_DISK_WRITE,
   /* a queued item compiled on a worker */
   STUTTER_WORKER_COMPILE,
   /* every shader and pipeline the backend made, on any thread */
   STUTTER_SHADER_CREATE,
   STUTTER_PIPELINE_CREATE,
   /* a pipeline the driver compiled or linked, on any thread: a pipeline
    * made from one already compiled is not one */
   STUTTER_DRIVER_PIPELINE,
   /* one of those compiled or linked for a GX or ubershader draw, which
    * waited for it */
   STUTTER_DRAW_DRIVER_PIPELINE,
   STUTTER_STAT_COUNT
};

#ifdef HAVE_STUTTER_STATS
void stutter_stat_add(enum stutter_stat stat, retro_perf_tick_t cycles);
/* This thread builds for a draw from here, and no longer from here. */
void stutter_draw_enter(void);
void stutter_draw_leave(void);
/* A pipeline the driver compiled or linked, by where it was. */
void stutter_driver_pipeline(retro_perf_tick_t cycles);

/* The counts and cycles so far, of the first n kinds (enum stutter_stat,
 * in its order); the number of kinds this build counts. The harness's
 * entry point, exported by the stats build alone. */
RETRO_API unsigned parallel_gx1_stutter_stats(unsigned long long *counts,
      unsigned long long *cycles, unsigned n);

/* A tick where the work starts, then one more of that kind and the
 * cycles since it, where it ends. */
#define STUTTER_MARK(t) const retro_perf_tick_t t = cpu_features_get_perf_counter()
#define STUTTER_SINCE(stat, t) stutter_stat_add((stat), cpu_features_get_perf_counter() - (t))
/* one more of that kind, uncounted cycles */
#define STUTTER_COUNT(stat) stutter_stat_add((stat), 0)
/* the same, for building a draw waits for: what the driver compiles in
 * between is counted as the draw's */
#define STUTTER_DRAW_MARK(t) \
   const retro_perf_tick_t t = (stutter_draw_enter(), cpu_features_get_perf_counter())
#define STUTTER_DRAW_SINCE(stat, t) (stutter_draw_leave(), STUTTER_SINCE(stat, t))
/* a pipeline the driver compiled or linked since t */
#define STUTTER_DRIVER_SINCE(t) stutter_driver_pipeline(cpu_features_get_perf_counter() - (t))
#else
#define STUTTER_MARK(t) ((void)0)
#define STUTTER_SINCE(stat, t) ((void)0)
#define STUTTER_COUNT(stat) ((void)0)
#define STUTTER_DRAW_MARK(t) ((void)0)
#define STUTTER_DRAW_SINCE(stat, t) ((void)0)
#define STUTTER_DRIVER_SINCE(t) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
