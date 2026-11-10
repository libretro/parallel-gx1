// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/OGLShader.h"

#include "Common/Logging/Log.h"
#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/ShaderArena.h"
#include "VideoCommon/ShaderGenCommon.h"

#include "VideoCommon/VideoConfig.h"

namespace OGL
{
static GLenum GetGLShaderTypeForStage(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return GL_VERTEX_SHADER;
  case ShaderStage::Geometry:
    return GL_GEOMETRY_SHADER;
  case ShaderStage::Pixel:
    return GL_FRAGMENT_SHADER;
  case ShaderStage::Compute:
    return GL_COMPUTE_SHADER;
  default:
    return 0;
  }
}

/* the label is copied by GL, from the borrowed name and its length */
OGLShader::OGLShader(ShaderStage stage, GLenum gl_type, GLuint gl_id, shader_text source,
                     std::string_view name)
    : AbstractShader(stage), m_id(ProgramShaderCache::GenerateShaderID()), m_type(gl_type),
      m_gl_id(gl_id), m_source(source)
{
  if (!name.empty() && g_backend_info.bSupportsSettingObjectNames)
    glObjectLabel(GL_SHADER, m_gl_id, (GLsizei)name.size(), name.data());
}

OGLShader::OGLShader(GLuint gl_compute_program_id, shader_text source, std::string_view name)
    : AbstractShader(ShaderStage::Compute), m_id(ProgramShaderCache::GenerateShaderID()),
      m_type(GL_COMPUTE_SHADER), m_gl_compute_program_id(gl_compute_program_id),
      m_source(source)
{
  if (!name.empty() && g_backend_info.bSupportsSettingObjectNames)
    glObjectLabel(GL_PROGRAM, m_gl_compute_program_id, (GLsizei)name.size(), name.data());
}

OGLShader::~OGLShader()
{
  shader_text_free(&m_source);
  if (m_stage != ShaderStage::Compute)
    glDeleteShader(m_gl_id);
  else
    glDeleteProgram(m_gl_compute_program_id);
}

OGLShader* OGLShader::CreateFromSource(ShaderStage stage, ShaderCode* source,
                                                       std::string_view name)
{
  if (stage != ShaderStage::Compute)
  {
    GLenum shader_type = GetGLShaderTypeForStage(stage);
    GLuint shader_id = ProgramShaderCache::CompileSingleShader(shader_type, source->GetBuffer());
    if (!shader_id)
      return nullptr;

    return new OGLShader(stage, shader_type, shader_id, source->Release(), name);
  }

  // Compute shaders.
  SHADER prog;
  if (!ProgramShaderCache::CompileComputeShader(prog, source->GetBuffer()))
    return nullptr;
  return new OGLShader(prog.glprogid, source->Release(), name);
}

OGLShader* OGLShader::CreateFromBuild(ShaderStage stage, shader_build_fn fn, const void* arg,
                                      std::string_view name)
{
  char err[1024];
  struct rslc_opts opts;
  struct rslc_result result;
  ShaderCode code;
  bool es;
  unsigned flags = 0;
  const enum rslc_stage rstage = stage == ShaderStage::Vertex   ? RSLC_STAGE_VERTEX :
                                 stage == ShaderStage::Geometry ? RSLC_STAGE_GEOMETRY :
                                 stage == ShaderStage::Pixel    ? RSLC_STAGE_FRAGMENT :
                                                                  RSLC_STAGE_COMPUTE;
  memset(&opts, 0, sizeof(opts));
  opts.version = ProgramShaderCache::GLSLVersionNumber(&es);
  opts.dst = es ? RSLC_DST_GLSL_ES : RSLC_DST_GLSL;
  opts.err = err;
  opts.err_cap = sizeof(err);
  /* what the text's header said of the context */
  opts.fb_fetch = g_ogl_config.SupportedFramebufferFetch == EsFbFetchType::FbFetchExt ?
                      RSLC_FB_FETCH_EXT :
                  g_ogl_config.SupportedFramebufferFetch == EsFbFetchType::FbFetchArm ?
                      RSLC_FB_FETCH_ARM :
                      RSLC_FB_FETCH_NONE;
  opts.early_z_depth_unchanged =
      !g_ogl_config.bSupportsImageLoadStore && g_ogl_config.bSupportsConservativeDepth;
  if (g_backend_info.bSupportsBindingLayout)
    flags |= SHADER_BUILD_GL_BINDINGS;
  if (g_ogl_config.bSupportsExplicitLayoutInShader)
    flags |= SHADER_BUILD_GL_LOCATIONS;
  if (g_ogl_config.bSupportsKHRShaderSubgroup)
    flags |= SHADER_BUILD_SUBGROUP;
  if (!shader_build_compile(fn, arg, rstage, SHADER_BUILD_OPENGL, flags, &opts, &result))
  {
    ERROR_LOG_FMT(VIDEO, "rslc could not build a shader: {}", err);
    return nullptr;
  }
  code.Putn(result.text, result.text_len);
  shader_arena_blob_release(result.blob);

  if (stage != ShaderStage::Compute)
  {
    GLenum shader_type = GetGLShaderTypeForStage(stage);
    GLuint shader_id =
        ProgramShaderCache::CompileSingleShader(shader_type, code.GetBuffer(), "", false);
    if (!shader_id)
      return nullptr;
    return new OGLShader(stage, shader_type, shader_id, code.Release(), name);
  }

  SHADER prog;
  if (!ProgramShaderCache::CompileComputeShader(prog, code.GetBuffer(), false))
    return nullptr;
  return new OGLShader(prog.glprogid, code.Release(), name);
}

}  // namespace OGL
