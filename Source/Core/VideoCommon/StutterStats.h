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
   STUTTER_STAT_COUNT
};

#ifdef HAVE_STUTTER_STATS
void stutter_stat_add(enum stutter_stat stat, retro_perf_tick_t cycles);

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
#else
#define STUTTER_MARK(t) ((void)0)
#define STUTTER_SINCE(stat, t) ((void)0)
#define STUTTER_COUNT(stat) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
