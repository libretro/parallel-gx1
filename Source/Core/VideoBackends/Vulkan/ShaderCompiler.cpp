// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/ShaderCompiler.h"

#include <cstddef>
#include <string>

#include <cstdio>
#include <cstring>

#include <gfx/shaders/rslc.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Vulkan::ShaderCompiler
{
// Regarding the UBO bind points, we subtract one from the binding index because
// the OpenGL backend requires UBO #0 for non-block uniforms (at least on NV).
// This allows us to share the same shaders but use bind point #0 in the Vulkan
// backend. None of the Vulkan-specific shaders use UBOs, instead they use push
// constants, so when/if the GL backend moves to uniform blocks completely this
// subtraction can be removed.
static const char SHADER_HEADER[] = R"(
  // Target GLSL 4.5.
  #version 450 core

  #extension GL_ARB_shading_language_include : enable

  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(set = 1, binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 8))
  #define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
  #define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in

  // Metal framebuffer fetch helpers.
  #define FB_FETCH_VALUE subpassLoad(in_ocol0)

  // hlsl to glsl function translation
  #define API_VULKAN 1
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

  // These were changed in Vulkan
  #define gl_VertexID gl_VertexIndex
  #define gl_InstanceID gl_InstanceIndex
)";
static const char COMPUTE_SHADER_HEADER[] = R"(
  // Target GLSL 4.5.
  #version 450 core
  // All resources are packed into one descriptor set for compute.
  #define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(set = 0, binding = (1 + x))
  #define TEXEL_BUFFER_BINDING(x) layout(set = 0, binding = (9 + x))
  #define IMAGE_BINDING(format, x) layout(format, set = 0, binding = (11 + x))

  // hlsl to glsl function translation
  #define API_VULKAN 1
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
)";
/* The stage's full text: the header, then the generator's source. Subgroup
 * operations are not offered to the generators: rslc has no form of them. */
static std::string GetShaderCode(std::string_view source, std::string_view header)
{
  std::string full_source_code;
  full_source_code.reserve(header.size() + source.size());
  full_source_code.append(header);
  if (DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER))
  {
    full_source_code.append("#define gl_HelperInvocation !gl_HelperInvocation "
                            "// Work around broken AMD Metal driver\n");
  }
  full_source_code.append(source);
  return full_source_code;
}

/* A failed stage is logged and written to the dump directory with the error. */
static void ReportFailure(const char* stage_name, const std::string& source, const char* err)
{
  static int counter;
  std::string path = VideoBackendBase::BadShaderFilename(stage_name, counter++);
  FILE* f;

  ERROR_LOG_FMT(VIDEO, "rslc could not compile a {} shader: {}", stage_name, err);
  File::CreateFullPath(path);
  f = fopen(path.c_str(), "wb");
  if (!f)
    return;
  fwrite(source.data(), 1, source.size(), f);
  fprintf(f, "\n/* rslc: %s */\n", err);
  fclose(f);
}

static std::optional<SPIRVCodeVector> Compile(std::string_view source_code, enum rslc_stage stage,
                                              const char* stage_name)
{
  char err[1024];
  struct rslc_opts opts;
  struct rslc_result result;
  std::string source = GetShaderCode(
      source_code, stage == RSLC_STAGE_COMPUTE ? COMPUTE_SHADER_HEADER : SHADER_HEADER);
  std::optional<SPIRVCodeVector> spv;

  memset(&opts, 0, sizeof(opts));
  opts.dst = RSLC_DST_SPIRV;
  opts.err = err;
  opts.err_cap = sizeof(err);
  err[0] = '\0';
  if (!rslc_compile(source.data(), source.size(), stage, &opts, &result))
  {
    ReportFailure(stage_name, source, err);
    return spv;
  }
  spv = SPIRVCodeVector(result.spirv, result.spirv + result.spirv_len);
  rslc_result_free(&result, &opts);
  return spv;
}

std::optional<SPIRVCodeVector> CompileVertexShader(std::string_view source_code)
{
  return Compile(source_code, RSLC_STAGE_VERTEX, "vs");
}

std::optional<SPIRVCodeVector> CompileGeometryShader(std::string_view source_code)
{
  return Compile(source_code, RSLC_STAGE_GEOMETRY, "gs");
}

std::optional<SPIRVCodeVector> CompileFragmentShader(std::string_view source_code)
{
  return Compile(source_code, RSLC_STAGE_FRAGMENT, "ps");
}

std::optional<SPIRVCodeVector> CompileComputeShader(std::string_view source_code)
{
  return Compile(source_code, RSLC_STAGE_COMPUTE, "cs");
}
}  // namespace Vulkan::ShaderCompiler
