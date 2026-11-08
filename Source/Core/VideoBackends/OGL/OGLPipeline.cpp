// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/OGLPipeline.h"

#include <utility>

#include "VideoBackends/OGL/OGLShader.h"
#include "VideoBackends/OGL/OGLVertexManager.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
static GLenum MapToGLPrimitive(PrimitiveType primitive_type)
{
  switch (primitive_type)
  {
  case PrimitiveType::Points:
    return GL_POINTS;
  case PrimitiveType::Lines:
    return GL_LINES;
  case PrimitiveType::Triangles:
    return GL_TRIANGLES;
  case PrimitiveType::TriangleStrip:
    return GL_TRIANGLE_STRIP;
  default:
    return 0;
  }
}
OGLPipeline::OGLPipeline(const AbstractPipelineConfig& config, const GLVertexFormat* vertex_format,
                         RasterizationState rasterization_state, DepthState depth_state,
                         BlendingState blending_state, PipelineProgram* program,
                         GLuint gl_primitive)
    : AbstractPipeline(config), m_vertex_format(vertex_format),
      m_rasterization_state(std::move(rasterization_state)), m_depth_state(std::move(depth_state)),
      m_blending_state(std::move(blending_state)), m_program(program), m_gl_primitive(gl_primitive)
{
}

OGLPipeline::~OGLPipeline()
{
  // We don't want to destroy the shaders.
  ProgramShaderCache::ReleasePipelineProgram(m_program);
}

size_t OGLPipeline::WriteCacheData(CacheDataReserve reserve, void* ctx) const
{
  // More than one pipeline can share the same shaders. To avoid bloating the cache with multiple
  // copies of the same program combination, we set a flag on the program object so that it can't
  // be retrieved again. When booting, the pipeline cache is loaded in-order, so the additional
  // pipelines which use the program combination will re-use the already-created object.
  if (!g_backend_info.bSupportsPipelineCacheData || m_program->binary_retrieved ||
      !ProgramShaderCache::FinishLink(m_program, true))
  {
    return 0;
  }

  GLint program_size = 0;
  glGetProgramiv(m_program->shader.glprogid, GL_PROGRAM_BINARY_LENGTH, &program_size);
  if (program_size == 0)
    return 0;

  // Clear any existing error.
  glGetError();

  // The format first, then the binary, which the driver writes into the store.
  u8* const data = reserve(ctx, program_size + sizeof(u32));
  if (!data)
    return 0;
  GLsizei data_size = 0;
  GLenum program_format = 0;
  glGetProgramBinary(m_program->shader.glprogid, program_size, &data_size, &program_format,
                     data + sizeof(u32));
  if (glGetError() != GL_NO_ERROR || data_size == 0)
    return 0;

  u32 program_format_u32 = static_cast<u32>(program_format);
  std::memcpy(data, &program_format_u32, sizeof(u32));
  m_program->binary_retrieved = true;
  return data_size + sizeof(u32);
}

OGLPipeline* OGLPipeline::Create(const AbstractPipelineConfig& config,
                                                 const void* cache_data, size_t cache_data_size)
{
  PipelineProgram* program = ProgramShaderCache::GetPipelineProgram(
      static_cast<const GLVertexFormat*>(config.vertex_format),
      static_cast<const OGLShader*>(config.vertex_shader),
      static_cast<const OGLShader*>(config.geometry_shader),
      static_cast<const OGLShader*>(config.pixel_shader), cache_data, cache_data_size);
  if (!program)
    return nullptr;

  const GLVertexFormat* vertex_format = static_cast<const GLVertexFormat*>(config.vertex_format);
  GLenum gl_primitive = MapToGLPrimitive(config.rasterization_state.primitive);
  return new OGLPipeline(config, vertex_format, config.rasterization_state,
                                       config.depth_state, config.blending_state, program,
                                       gl_primitive);
}
}  // namespace OGL
