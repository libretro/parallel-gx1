// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Metal/MTLPerfQuery.h"

#include "VideoBackends/Metal/MTLStateTracker.h"

void Metal::PerfQuery::EnableQuery(PerfQueryGroup type)
{
  if (type == PQG_ZCOMP_ZCOMPLOC || type == PQG_ZCOMP)
    g_state_tracker->EnablePerfQuery(type, m_current_query);
}

void Metal::PerfQuery::DisableQuery(PerfQueryGroup type)
{
  if (type == PQG_ZCOMP_ZCOMPLOC || type == PQG_ZCOMP)
    g_state_tracker->DisablePerfQuery();
}

void Metal::PerfQuery::ResetQuery()
{
  std::lock_guard<std::mutex> lock(m_results_mtx);
  m_current_query++;
  ResetQueryResults();
}

u32 Metal::PerfQuery::GetQueryResult(PerfQueryType type)
{
  u32 result = 0;
  if (type == PQ_ZCOMP_INPUT_ZCOMPLOC || type == PQ_ZCOMP_OUTPUT_ZCOMPLOC)
  {
    result = QueryResult(PQG_ZCOMP_ZCOMPLOC);
  }
  else if (type == PQ_ZCOMP_INPUT || type == PQ_ZCOMP_OUTPUT)
  {
    result = QueryResult(PQG_ZCOMP);
  }
  else if (type == PQ_BLEND_INPUT)
  {
    result = QueryResult(PQG_ZCOMP) + QueryResult(PQG_ZCOMP_ZCOMPLOC);
  }
  else if (type == PQ_EFB_COPY_CLOCKS)
  {
    result = QueryResult(PQG_EFB_COPY_CLOCKS);
  }

  return result / 4;
}

void Metal::PerfQuery::FlushResults()
{
  if (IsFlushed())
    return;

  // There's a possibility that some active performance queries are unflushed
  g_state_tracker->FlushEncoders();
  g_state_tracker->NotifyOfCPUGPUSync();

  std::unique_lock<std::mutex> lock(m_results_mtx);
  while (!IsFlushed())
    m_cv.wait(lock);
}

bool Metal::PerfQuery::IsFlushed() const
{
  return QueryCount() == 0;
}

void Metal::PerfQuery::ReturnResults(const u64* data, const PerfQueryGroup* groups, size_t count,
                                     u32 query_id)
{
  {
    std::lock_guard<std::mutex> lock(m_results_mtx);
    if (m_current_query == query_id)
    {
      for (size_t i = 0; i < count; ++i)
      {
        u64 native_res_result =
            data[i] * (EFB_WIDTH * EFB_HEIGHT) /
            (g_framebuffer_manager->GetEFBWidth() * g_framebuffer_manager->GetEFBHeight());

        native_res_result /= g_ActiveConfig.iMultisamples;

        AddQueryResult(groups[i], static_cast<u32>(native_res_result));
      }
    }
    SubQueryCount(1);
  }
  m_cv.notify_one();
}
