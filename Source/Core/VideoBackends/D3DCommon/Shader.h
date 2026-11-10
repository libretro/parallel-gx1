// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBinary.h"
#include "VideoCommon/ShaderBuild.h"

class ShaderCode;

namespace D3DCommon
{
class Shader : public AbstractShader
{
public:
  ~Shader();

  const u8* GetByteCode() const { return m_bytecode.code; }
  size_t GetByteCodeSize() const { return m_bytecode.size; }

  const u8* GetBinary(size_t* size) const
  {
    *size = m_bytecode.size;
    return m_bytecode.code;
  }

  /* Compiles a stage to DXBC into *bytecode, which owns rslc's result
   * block then. False, with the failure logged and dumped, if rslc
   * refused it. */
  static bool CompileShader(D3D_FEATURE_LEVEL feature_level, ShaderStage stage,
                            ShaderCode* source, ShaderBinary* bytecode);
  /* The same for a stage fn builds by calls. */
  static bool CompileBuild(D3D_FEATURE_LEVEL feature_level, ShaderStage stage,
                           shader_build_fn fn, const void* arg, ShaderBinary* bytecode);

protected:
  /* Takes the bytecode's block. */
  Shader(ShaderStage stage, ShaderBinary bytecode);

  ShaderBinary m_bytecode;
};

}  // namespace D3DCommon
