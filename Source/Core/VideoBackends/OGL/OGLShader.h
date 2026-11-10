// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBuild.h"
#include "VideoCommon/ShaderText.h"

class ShaderCode;

namespace OGL
{
class OGLShader final : public AbstractShader
{
public:
  /* Both take the source's block. */
  explicit OGLShader(ShaderStage stage, GLenum gl_type, GLuint gl_id, shader_text source,
                     std::string_view name);
  explicit OGLShader(GLuint gl_compute_program_id, shader_text source, std::string_view name);
  ~OGLShader();

  u64 GetID() const { return m_id; }
  GLenum GetGLShaderType() const { return m_type; }
  GLuint GetGLShaderID() const { return m_gl_id; }
  GLuint GetGLComputeProgramID() const { return m_gl_compute_program_id; }
  std::string_view GetSource() const
  {
    return std::string_view(shader_text_data(&m_source), shader_text_len(&m_source));
  }

  /* Takes the source's text: the shader keeps it for its link's messages. */
  static OGLShader* CreateFromSource(ShaderStage stage, ShaderCode* source,
                                                     std::string_view name);
  /* A stage fn builds by calls, printed as the context's GLSL. */
  static OGLShader* CreateFromBuild(ShaderStage stage, shader_build_fn fn, const void* arg,
                                    std::string_view name);

private:
  u64 m_id;
  GLenum m_type;
  GLuint m_gl_id = 0;
  GLuint m_gl_compute_program_id = 0;
  shader_text m_source;
};

}  // namespace OGL
