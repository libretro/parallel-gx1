// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

enum class ShaderStage
{
  Vertex,
  Geometry,
  Pixel,
  Compute
};

class AbstractShader
{
public:
  explicit AbstractShader(ShaderStage stage) : m_stage(stage) {}

  ShaderStage GetStage() const { return m_stage; }

  // Shader binaries represent the input source code in a lower-level form. e.g. SPIR-V or DXBC.
  // The shader source code is not required to create a shader object from the binary.
  // Lent for the shader's lifetime: nullptr with *size 0 where there is none.
  const u8* GetBinary(size_t* size) const;

protected:
  ShaderStage m_stage;
};

/* Deletes a shader as the backend's own class; NULL is let be. */
void AbstractShaderDelete(AbstractShader* shader);
