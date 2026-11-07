// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractDeleters.h"

enum PerfQueryType
{
  PQ_ZCOMP_INPUT_ZCOMPLOC = 0,
  PQ_ZCOMP_OUTPUT_ZCOMPLOC,
  PQ_ZCOMP_INPUT,
  PQ_ZCOMP_OUTPUT,
  PQ_BLEND_INPUT,
  PQ_EFB_COPY_CLOCKS,
  PQ_NUM_MEMBERS
};

enum PerfQueryGroup
{
  PQG_ZCOMP_ZCOMPLOC,
  PQG_ZCOMP,
  PQG_EFB_COPY_CLOCKS,
  PQG_NUM_MEMBERS,
};

/* The backend's queries are called by the backend in use
 * (BackendPixelEngine.cpp); with none, nothing is counted. */
class PerfQueryBase
{
public:
  PerfQueryBase() {}
  ~PerfQueryBase() {}

  bool Initialize();

  // Checks if performance queries are enabled in the gameini configuration.
  // NOTE: Called from CPU+GPU thread
  static bool ShouldEmulate();

  // Begin querying the specified value for the following host GPU commands
  // The call to EnableQuery() should be placed immediately before the draw command, otherwise
  // there is a risk of GPU resets if the query is left open and the buffer is submitted during
  // resource binding (D3D12/Vulkan).
  void EnableQuery(PerfQueryGroup type);

  // Stop querying the specified value for the following host GPU commands
  void DisableQuery(PerfQueryGroup type);

  // Reset query counters to zero and drop any pending queries
  void ResetQuery();

  // Return the measured value for the specified query type
  // NOTE: Called from CPU thread
  u32 GetQueryResult(PerfQueryType type);

  // Request the value of any pending queries - causes a pipeline flush and thus should be used
  // carefully!
  void FlushResults();

  // True if there are no further pending query results
  // NOTE: Called from CPU thread
  bool IsFlushed() const;

protected:
  /* The GPU thread retires queries while the CPU thread reads results: u32s in atomic words. */
  u32 QueryCount() const { return static_cast<u32>(retro_atomic_load_acquire_int(&m_query_count)); }
  void AddQueryCount(u32 count)
  {
    retro_atomic_fetch_add_int(&m_query_count, static_cast<int>(count));
  }
  void SubQueryCount(u32 count)
  {
    retro_atomic_fetch_sub_int(&m_query_count, static_cast<int>(count));
  }
  u32 QueryResult(PerfQueryGroup group) const
  {
    return static_cast<u32>(retro_atomic_load_acquire_int(&m_results[group]));
  }
  void AddQueryResult(PerfQueryGroup group, u32 value)
  {
    retro_atomic_fetch_add_int(&m_results[group], static_cast<int>(value));
  }
  void SetQueryResult(PerfQueryGroup group, u32 value)
  {
    retro_atomic_store_release_int(&m_results[group], static_cast<int>(value));
  }
  /* Drops the pending queries and zeroes the results. */
  void ResetQueryState()
  {
    int i;
    retro_atomic_store_release_int(&m_query_count, 0);
    for (i = 0; i < PQG_NUM_MEMBERS; i++)
      retro_atomic_store_release_int(&m_results[i], 0);
  }
  void ResetQueryResults()
  {
    int i;
    for (i = 0; i < PQG_NUM_MEMBERS; i++)
      retro_atomic_store_release_int(&m_results[i], 0);
  }

  retro_atomic_int_t m_query_count{};
  retro_atomic_int_t m_results[PQG_NUM_MEMBERS]{};
};

extern std::unique_ptr<PerfQueryBase> g_perf_query;
