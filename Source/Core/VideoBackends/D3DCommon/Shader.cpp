// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3DCommon/Shader.h"

#include <optional>
#include <string>
#include <string_view>

#include <cstdio>
#include <cstring>

#include <gfx/shaders/rslc.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "VideoCommon/VideoBackendBase.h"

namespace
{
// Regarding the UBO bind points, we subtract one from the binding index because
// the OpenGL backend requires UBO #0 for non-block uniforms (at least on NV).
// This allows us to share the same shaders but use bind point #0 in the D3D
// backends. None of the specific shaders use UBOs, instead they use push
// constants, so when/if the GL backend moves to uniform blocks completely this
// subtraction can be removed.
constexpr std::string_view SHADER_HEADER = R"(
  // Target GLSL 4.5.
  #version 450 core

  #extension GL_ARB_shading_language_include : enable

  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = x)
  #define SSBO_BINDING(x) layout(binding = (x + 2))
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in

  // hlsl to glsl function translation
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix

  #define API_D3D 1

  // rslc reads the Vulkan names of these
  #define gl_VertexID gl_VertexIndex
  #define gl_InstanceID gl_InstanceIndex
)";
constexpr std::string_view COMPUTE_SHADER_HEADER = R"(
  // Target GLSL 4.5.
  #version 450 core
  // All resources are packed into one descriptor set for compute.
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = x)
  #define IMAGE_BINDING(format, x) layout(format, binding = x)

  // hlsl to glsl function translation
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix

  #define API_D3D 1
)";

/* A stage's bytecode, compiled by rslc from the GLSL the generators write
 * to shader model 4.0, 4.1 or 5.0 by the feature level. A stage rslc
 * refuses is logged and written to the dump directory with the reason. */
std::optional<AbstractShader::BinaryData> CompileWithRslc(D3D_FEATURE_LEVEL feature_level,
                                                          ShaderStage stage,
                                                          std::string_view source)
{
  static int num_failures;
  char err[1024];
  struct rslc_opts opts;
  struct rslc_result result;
  enum rslc_stage rstage;
  std::string full;
  std::optional<AbstractShader::BinaryData> code;
  FILE* f;

  rstage = stage == ShaderStage::Vertex   ? RSLC_STAGE_VERTEX :
           stage == ShaderStage::Pixel    ? RSLC_STAGE_FRAGMENT :
           stage == ShaderStage::Geometry ? RSLC_STAGE_GEOMETRY :
                                            RSLC_STAGE_COMPUTE;
  full.append(stage == ShaderStage::Compute ? COMPUTE_SHADER_HEADER : SHADER_HEADER);
  full.append(source);

  memset(&opts, 0, sizeof(opts));
  opts.dst = RSLC_DST_DXBC;
  opts.version = feature_level == D3D_FEATURE_LEVEL_10_0 ? 40 :
                 feature_level == D3D_FEATURE_LEVEL_10_1 ? 41 :
                                                           50;
  opts.err = err;
  opts.err_cap = sizeof(err);
  err[0] = '\0';
  if (rslc_compile(full.data(), full.size(), rstage, &opts, &result))
  {
    {
      const u8* bytes = reinterpret_cast<const u8*>(result.spirv);
      code = AbstractShader::BinaryData(bytes, bytes + result.spirv_len * sizeof(uint32_t));
    }
    rslc_result_free(&result, &opts);
    return code;
  }

  ERROR_LOG_FMT(VIDEO, "rslc could not compile a shader: {}", err);
  {
    std::string path = VideoBackendBase::BadShaderFilename("rslc", num_failures++);
    File::CreateFullPath(path);
    f = fopen(path.c_str(), "wb");
    if (f)
    {
      fwrite(full.data(), 1, full.size(), f);
      fprintf(f, "\n/* rslc: %s */\n", err);
      fclose(f);
    }
  }
  return code;
}
}  // namespace

namespace D3DCommon
{
Shader::Shader(ShaderStage stage, BinaryData bytecode)
    : AbstractShader(stage), m_bytecode(std::move(bytecode))
{
}

Shader::~Shader() = default;

AbstractShader::BinaryData Shader::GetBinary() const
{
  return m_bytecode;
}

std::optional<Shader::BinaryData>
Shader::CompileShader(D3D_FEATURE_LEVEL feature_level, ShaderStage stage, std::string_view source)
{
  return CompileWithRslc(feature_level, stage, source);
}

AbstractShader::BinaryData Shader::CreateByteCode(const void* data, size_t length)
{
  const auto* const begin = static_cast<const u8*>(data);
  const auto* const end = begin + length;

  return {begin, end};
}

}  // namespace D3DCommon
