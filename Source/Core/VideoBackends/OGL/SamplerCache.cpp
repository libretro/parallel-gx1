// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/SamplerCache.h"

#include <cstdlib>
#include <cstring>

#include "Common/CommonTypes.h"
#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
SamplerCache* g_sampler_cache = nullptr;

SamplerCache::SamplerCache()
{
  glGenSamplers(1, &m_point_sampler);
  glGenSamplers(1, &m_linear_sampler);
  glSamplerParameteri(m_point_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_point_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(m_point_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(m_point_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(m_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(m_linear_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(m_linear_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

SamplerCache::~SamplerCache()
{
  Clear();
  std::free(m_cache);
  glDeleteSamplers(1, &m_point_sampler);
  glDeleteSamplers(1, &m_linear_sampler);
}

void SamplerCache::SetSamplerState(u32 stage, const SamplerState& state)
{
  if (m_active_samplers[stage].state == state && m_active_samplers[stage].sampler != 0)
    return;

  const u64 key = state.Hex();
  u32 low = 0;
  u32 high = m_cache_count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (m_cache[mid].key < key)
      low = mid + 1;
    else
      high = mid;
  }

  GLuint sampler;
  if (low < m_cache_count && m_cache[low].key == key)
  {
    sampler = m_cache[low].sampler;
  }
  else
  {
    glGenSamplers(1, &sampler);
    SetParameters(sampler, state);
    if (m_cache_count == m_cache_capacity)
    {
      const u32 capacity = m_cache_capacity ? m_cache_capacity * 2 : 64;
      CachedSampler* const grown =
          static_cast<CachedSampler*>(std::realloc(m_cache, capacity * sizeof(CachedSampler)));
      if (grown)
      {
        m_cache = grown;
        m_cache_capacity = capacity;
      }
    }
    // out of memory, the sampler goes uncached
    if (m_cache_count < m_cache_capacity)
    {
      std::memmove(m_cache + low + 1, m_cache + low, (m_cache_count - low) * sizeof(CachedSampler));
      m_cache[low].key = key;
      m_cache[low].sampler = sampler;
      m_cache_count++;
    }
  }

  m_active_samplers[stage].state = state;
  m_active_samplers[stage].sampler = sampler;
  glBindSampler(stage, sampler);
}

void SamplerCache::SetParameters(GLuint sampler_id, const SamplerState& params)
{
  GLenum min_filter;
  GLenum mag_filter = (params.tm0.mag_filter == FilterMode::Near) ? GL_NEAREST : GL_LINEAR;
  if (params.tm0.mipmap_filter == FilterMode::Linear)
  {
    min_filter = (params.tm0.min_filter == FilterMode::Near) ? GL_NEAREST_MIPMAP_LINEAR :
                                                               GL_LINEAR_MIPMAP_LINEAR;
  }
  else
  {
    min_filter = (params.tm0.min_filter == FilterMode::Near) ? GL_NEAREST_MIPMAP_NEAREST :
                                                               GL_LINEAR_MIPMAP_NEAREST;
  }

  glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER, min_filter);
  glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER, mag_filter);

  static constexpr GLenum address_modes[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};

  glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S,
                      address_modes[static_cast<u32>(params.tm0.wrap_u.Value())]);
  glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T,
                      address_modes[static_cast<u32>(params.tm0.wrap_v.Value())]);

  glSamplerParameterf(sampler_id, GL_TEXTURE_MIN_LOD, params.tm1.min_lod / 16.f);
  glSamplerParameterf(sampler_id, GL_TEXTURE_MAX_LOD, params.tm1.max_lod / 16.f);

  if (g_backend_info.bSupportsLodBiasInSampler)
  {
    glSamplerParameterf(sampler_id, GL_TEXTURE_LOD_BIAS, params.tm0.lod_bias / 256.f);
  }

  if (params.tm0.anisotropic_filtering != 0 && g_ogl_config.bSupportsAniso)
  {
    glSamplerParameterf(sampler_id, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        static_cast<float>(1 << params.tm0.anisotropic_filtering));
  }
}

void SamplerCache::Clear()
{
  for (u32 i = 0; i < m_cache_count; i++)
    glDeleteSamplers(1, &m_cache[i].sampler);
  for (ActiveSampler& active : m_active_samplers)
    active.sampler = 0;
  m_cache_count = 0;
}
}  // namespace OGL
