/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef HAVE_STUTTER_STATS

#include <retro_atomic.h>

#include "VideoCommon/StutterStats.h"

static retro_atomic_size_t s_counts[STUTTER_STAT_COUNT];
static retro_atomic_size_t s_cycles[STUTTER_STAT_COUNT];

void stutter_stat_add(enum stutter_stat stat, retro_perf_tick_t cycles)
{
   retro_atomic_fetch_add_size(&s_counts[stat], 1);
   if (cycles)
      retro_atomic_fetch_add_size(&s_cycles[stat], (size_t)cycles);
}

RETRO_API unsigned parallel_gx1_stutter_stats(unsigned long long *counts,
      unsigned long long *cycles, unsigned n)
{
   unsigned i;
   for (i = 0; i < n && i < STUTTER_STAT_COUNT; i++)
   {
      counts[i] = retro_atomic_load_acquire_size(&s_counts[i]);
      cycles[i] = retro_atomic_load_acquire_size(&s_cycles[i]);
   }
   return STUTTER_STAT_COUNT;
}

#else

/* a translation unit is never empty */
typedef int stutter_stats_none;

#endif
