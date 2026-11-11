// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DPerfQuery.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
PerfQuery::PerfQuery() : m_query_read_pos()
{
  for (ActiveQuery& entry : m_query_buffer)
  {
    D3D11_QUERY_DESC qdesc = CD3D11_QUERY_DESC(D3D11_QUERY_OCCLUSION, 0);
    D3D::device->CreateQuery(&qdesc, &entry.query);
  }

  ResetQuery();
}

PerfQuery::~PerfQuery() = default;

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  u32 query_count = QueryCount();

  // Is this sane?
  if (query_count > m_query_buffer.size() / 2)
  {
    WeakFlush();
    query_count = QueryCount();
  }

  if (m_query_buffer.size() == query_count)
  {
    // TODO
    FlushOne();
    query_count = QueryCount();
    ERROR_LOG(VIDEO, "Flushed query buffer early!");
  }

  // start query
  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    auto& entry = m_query_buffer[(m_query_read_pos + query_count) % m_query_buffer.size()];

    D3D::context->Begin(entry.query.Get());
    entry.query_group = group;

    AddQueryCount(1);
  }
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  // stop query
  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    auto& entry = m_query_buffer[(m_query_read_pos + QueryCount() + m_query_buffer.size() - 1) %
                                 m_query_buffer.size()];
    D3D::context->End(entry.query.Get());
  }
}

void PerfQuery::ResetQuery()
{
  ResetQueryState();
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
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

void PerfQuery::FlushOne()
{
  auto& entry = m_query_buffer[m_query_read_pos];

  UINT64 result = 0;
  HRESULT hr = S_FALSE;
  while (hr != S_OK)
  {
    // TODO: Might cause us to be stuck in an infinite loop!
    hr = D3D::context->GetData(entry.query.Get(), &result, sizeof(result), 0);
  }

  // NOTE: Reported pixel metrics should be referenced to native resolution
  // TODO: Dropping the lower 2 bits from this count should be closer to actual
  // hardware behavior when drawing triangles.
  u64 native_res_result = result * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() * EFB_HEIGHT /
                          g_framebuffer_manager->GetEFBHeight();
  if (g_ActiveConfig.iMultisamples > 1)
    native_res_result /= g_ActiveConfig.iMultisamples;
  AddQueryResult(entry.query_group, static_cast<u32>(native_res_result));

  m_query_read_pos = (m_query_read_pos + 1) % m_query_buffer.size();
  SubQueryCount(1);
}

// TODO: could selectively flush things, but I don't think that will do much
void PerfQuery::FlushResults()
{
  while (!IsFlushed())
    FlushOne();
}

void PerfQuery::WeakFlush()
{
  while (!IsFlushed())
  {
    auto& entry = m_query_buffer[m_query_read_pos];

    UINT64 result = 0;
    HRESULT hr = D3D::context->GetData(entry.query.Get(), &result, sizeof(result),
                                       D3D11_ASYNC_GETDATA_DONOTFLUSH);

    if (hr == S_OK)
    {
      // NOTE: Reported pixel metrics should be referenced to native resolution
      const u64 native_res_result = result * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() *
                                    EFB_HEIGHT / g_framebuffer_manager->GetEFBHeight();
      SetQueryResult(entry.query_group, static_cast<u32>(native_res_result));

      m_query_read_pos = (m_query_read_pos + 1) % m_query_buffer.size();
      SubQueryCount(1);
    }
    else
    {
      break;
    }
  }
}

bool PerfQuery::IsFlushed() const
{
  return QueryCount() == 0;
}

}  // namespace DX11
