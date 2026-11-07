// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/PerfQueryBase.h"

namespace OGL
{
/* Occlusion queries: GL's sample counts, GLES's any-samples-passed, or the
 * GLES NV extension's pixel counts where it has them. */
class PerfQuery final : public PerfQueryBase
{
public:
  explicit PerfQuery(bool is_gles);
  ~PerfQuery();

  void EnableQuery(PerfQueryGroup group);
  void DisableQuery(PerfQueryGroup group);
  void ResetQuery();
  u32 GetQueryResult(PerfQueryType type);
  void FlushResults();
  bool IsFlushed() const;

private:
  struct ActiveQuery
  {
    GLuint query_id;
    PerfQueryGroup query_group;
  };

  // when testing in SMS: 64 was too small, 128 was ok
  static const u32 PERF_QUERY_BUFFER_SIZE = 512;

  void WeakFlush();
  // Only use when non-empty
  void FlushOne();

  // This contains gl query objects with unretrieved results.
  std::array<ActiveQuery, PERF_QUERY_BUFFER_SIZE> m_query_buffer;
  u32 m_query_read_pos = 0;

  GLenum m_query_type = 0;
  /* GL_NV_occlusion_query_samples on GLES */
  bool m_nv = false;
};

}  // namespace OGL
