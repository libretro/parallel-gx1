// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/OGLPerfQuery.h"

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
PerfQuery::PerfQuery(bool is_gles)
{
  u32 i;

  m_nv = is_gles && GLExtensions::Supports("GL_NV_occlusion_query_samples");
  m_query_type = is_gles ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED;
  for (i = 0; i < PERF_QUERY_BUFFER_SIZE; i++)
  {
    if (m_nv)
      glGenOcclusionQueriesNV(1, &m_query_buffer[i].query_id);
    else
      glGenQueries(1, &m_query_buffer[i].query_id);
  }
  ResetQuery();
}

PerfQuery::~PerfQuery()
{
  u32 i;

  for (i = 0; i < PERF_QUERY_BUFFER_SIZE; i++)
  {
    if (m_nv)
      glDeleteOcclusionQueriesNV(1, &m_query_buffer[i].query_id);
    else
      glDeleteQueries(1, &m_query_buffer[i].query_id);
  }
}

bool PerfQuery::IsFlushed() const
{
  return QueryCount() == 0;
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

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  u32 query_count = QueryCount();

  // Is this sane?
  if (query_count > PERF_QUERY_BUFFER_SIZE / 2)
  {
    WeakFlush();
    query_count = QueryCount();
  }

  if (query_count == PERF_QUERY_BUFFER_SIZE)
  {
    FlushOne();
    query_count = QueryCount();
  }

  // start query
  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    ActiveQuery& entry = m_query_buffer[(m_query_read_pos + query_count) % PERF_QUERY_BUFFER_SIZE];

    if (m_nv)
      glBeginOcclusionQueryNV(entry.query_id);
    else
      glBeginQuery(m_query_type, entry.query_id);
    entry.query_group = group;

    AddQueryCount(1);
  }
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  // stop query
  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    if (m_nv)
      glEndOcclusionQueryNV();
    else
      glEndQuery(m_query_type);
  }
}

void PerfQuery::WeakFlush()
{
  while (!IsFlushed())
  {
    const ActiveQuery& entry = m_query_buffer[m_query_read_pos];
    GLuint available = GL_FALSE;

    if (m_nv)
      glGetOcclusionQueryuivNV(entry.query_id, GL_PIXEL_COUNT_AVAILABLE_NV, &available);
    else
      glGetQueryObjectuiv(entry.query_id, GL_QUERY_RESULT_AVAILABLE, &available);
    if (available != GL_TRUE)
      break;
    FlushOne();
  }
}

void PerfQuery::FlushOne()
{
  const ActiveQuery& entry = m_query_buffer[m_query_read_pos];
  GLuint result = 0;
  u64 native_res_result;

  if (m_nv)
    glGetOcclusionQueryuivNV(entry.query_id, GL_OCCLUSION_TEST_RESULT_HP, &result);
  else
    glGetQueryObjectuiv(entry.query_id, GL_QUERY_RESULT, &result);

  // NOTE: Reported pixel metrics should be referenced to native resolution
  // TODO: Dropping the lower 2 bits from this count should be closer to actual
  // hardware behavior when drawing triangles.
  native_res_result = static_cast<u64>(result) * EFB_WIDTH * EFB_HEIGHT /
                      (g_framebuffer_manager->GetEFBWidth() * g_framebuffer_manager->GetEFBHeight());

  // Adjust for multisampling
  if (!m_nv && g_ActiveConfig.iMultisamples > 1)
    native_res_result /= g_ActiveConfig.iMultisamples;

  AddQueryResult(entry.query_group, static_cast<u32>(native_res_result));

  m_query_read_pos = (m_query_read_pos + 1) % PERF_QUERY_BUFFER_SIZE;
  SubQueryCount(1);
}

// TODO: could selectively flush things, but I don't think that will do much
void PerfQuery::FlushResults()
{
  while (!IsFlushed())
    FlushOne();
}

}  // namespace OGL
