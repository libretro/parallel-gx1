// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/RenderState.h"

namespace OGL
{
class SamplerCache
{
public:
  SamplerCache();
  ~SamplerCache();

  SamplerCache(const SamplerCache&) = delete;
  SamplerCache& operator=(const SamplerCache&) = delete;
  SamplerCache(SamplerCache&&) = delete;
  SamplerCache& operator=(SamplerCache&&) = delete;

  void SetSamplerState(u32 stage, const SamplerState& state);

  void Clear();

private:
  static void SetParameters(GLuint sampler_id, const SamplerState& params);

  // The samplers made, sorted by their state's bits
  struct CachedSampler
  {
    u64 key;
    GLuint sampler;
  };
  CachedSampler* m_cache = nullptr;
  u32 m_cache_count = 0;
  u32 m_cache_capacity = 0;

  struct ActiveSampler
  {
    SamplerState state;
    GLuint sampler;
  };
  ActiveSampler m_active_samplers[VideoCommon::MAX_PIXEL_SHADER_SAMPLERS]{};

  GLuint m_point_sampler;
  GLuint m_linear_sampler;
};

extern SamplerCache* g_sampler_cache;
}  // namespace OGL
