// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/ShaderCompiler.h"

#include <cstddef>
#include <string>

#include <cstdio>
#include <cstring>

#include <gfx/shaders/rslc.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/ShaderArena.h"
#include "VideoCommon/StutterStats.h"
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
/* The bounding box's subgroup reduction, where the device runs subgroup
 * operations in the pixel stage; the stages that use it are SPIR-V 1.3. */
static const char SUBGROUP_HELPER_HEADER[] =
    "#extension GL_KHR_shader_subgroup_basic : enable\n"
    "#extension GL_KHR_shader_subgroup_arithmetic : enable\n"
    "#define SUPPORTS_SUBGROUP_REDUCTION 1\n"
    "#define IS_HELPER_INVOCATION gl_HelperInvocation\n"
    "#define IS_FIRST_ACTIVE_INVOCATION (subgroupElect())\n"
    "#define SUBGROUP_MIN(value) value = subgroupMin(value)\n"
    "#define SUBGROUP_MAX(value) value = subgroupMax(value)\n";

/* The stage's header: the base one, then what the device and its driver's bugs want. */
static size_t GetShaderHeader(char* head, size_t cap, bool compute)
{
  const char* const parts[4] = {
      compute ? COMPUTE_SHADER_HEADER : SHADER_HEADER,
      g_vulkan_context->SupportsShaderSubgroupOperations() ? SUBGROUP_HELPER_HEADER : "",
      DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER) ?
          "#define gl_HelperInvocation !gl_HelperInvocation "
          "// Work around broken AMD Metal driver\n" :
          "",
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD) ?
          "#define BROKEN_SUBGROUP_WITH_DISCARD 1\n" :
          ""};
  size_t len = 0;
  int i;

  for (i = 0; i < 4; i++)
  {
    const size_t n = strlen(parts[i]);
    if (len + n > cap)
      return 0;
    memcpy(head + len, parts[i], n);
    len += n;
  }
  return len;
}

/* A failed stage is logged and written to the dump directory with the error. */
static void ReportFailure(const char* stage_name, const char* text, size_t len, const char* err)
{
  static int counter;
  const std::string path = VideoBackendBase::BadShaderFilename(stage_name, counter++);

  ERROR_LOG_FMT(VIDEO, "rslc could not compile a {} shader: {}", stage_name, err);
  File::CreateFullPath(path);
  File::IOFile file(path, "wb");
  file.WriteBytes(text, len);
  file.WriteBytes("\n/* rslc: ", 10);
  file.WriteBytes(err, strlen(err));
  file.WriteBytes(" */\n", 4);
}

bool Compile(ShaderStage stage, ShaderCode* source, ShaderBinary* spirv)
{
  char err[1024];
  char head[SHADER_TEXT_HEAD_ROOM];
  struct rslc_opts opts;
  struct rslc_result result;
  const enum rslc_stage rstage = stage == ShaderStage::Vertex   ? RSLC_STAGE_VERTEX :
                                 stage == ShaderStage::Geometry ? RSLC_STAGE_GEOMETRY :
                                 stage == ShaderStage::Pixel    ? RSLC_STAGE_FRAGMENT :
                                                                  RSLC_STAGE_COMPUTE;
  const char* const stage_name = stage == ShaderStage::Vertex   ? "vs" :
                                 stage == ShaderStage::Geometry ? "gs" :
                                 stage == ShaderStage::Pixel    ? "ps" :
                                                                  "cs";
  const size_t head_len = GetShaderHeader(head, sizeof(head), rstage == RSLC_STAGE_COMPUTE);
  /* the header goes in front of the text, not the text after a copy of it */
  const char* const text = head_len ? source->Prepend(head, head_len) : nullptr;
  const size_t text_len = head_len + source->Size();

  ShaderBinaryInit(spirv);
  if (!text)
  {
    ERROR_LOG_FMT(VIDEO, "The {} shader's header does not fit in front of it", stage_name);
    return false;
  }
  memset(&opts, 0, sizeof(opts));
  opts.dst = RSLC_DST_SPIRV;
  opts.err = err;
  opts.err_cap = sizeof(err);
  STUTTER_MARK(t);
  /* in this thread's block, grown and compiled again where it was too small */
  for (;;)
  {
    err[0] = '\0';
    shader_arena_use(&opts);
    if (rslc_compile(text, text_len, rstage, &opts, &result))
      break;
    if (!shader_arena_grow(err))
    {
      ReportFailure(stage_name, text, text_len, err);
      return false;
    }
  }
  STUTTER_EMIT_SINCE(t, result.spirv_len);
  /* the result block is kept whole: the words point into it */
  spirv->block = result.blob;
  spirv->release = shader_arena_blob_release;
  spirv->code = reinterpret_cast<const u8*>(result.spirv);
  spirv->size = result.spirv_len * sizeof(uint32_t);
  return true;
}
bool CompileBuild(ShaderStage stage, shader_build_fn fn, const void* arg, ShaderBinary* spirv)
{
  char err[1024];
  struct rslc_opts opts;
  struct rslc_result result;
  const enum rslc_stage rstage = stage == ShaderStage::Vertex   ? RSLC_STAGE_VERTEX :
                                 stage == ShaderStage::Geometry ? RSLC_STAGE_GEOMETRY :
                                 stage == ShaderStage::Pixel    ? RSLC_STAGE_FRAGMENT :
                                                                  RSLC_STAGE_COMPUTE;
  /* what the text's header says of the device and its driver's bugs */
  const unsigned flags =
      (g_vulkan_context->SupportsShaderSubgroupOperations() ? SHADER_BUILD_SUBGROUP : 0u) |
      (DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER) ?
           SHADER_BUILD_INVERTED_HELPER :
           0u) |
      (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD) ?
           SHADER_BUILD_SUBGROUP_BROKEN_DISCARD :
           0u);
  ShaderBinaryInit(spirv);
  memset(&opts, 0, sizeof(opts));
  opts.dst = RSLC_DST_SPIRV;
  opts.err = err;
  opts.err_cap = sizeof(err);
  STUTTER_MARK(t);
  if (!shader_build_compile(fn, arg, rstage, SHADER_BUILD_VULKAN, flags, &opts, &result))
  {
    ERROR_LOG_FMT(VIDEO, "rslc could not build a shader: {}", err);
    return false;
  }
  STUTTER_EMIT_SINCE(t, result.spirv_len);
  /* the result block is kept whole: the words point into it */
  spirv->block = result.blob;
  spirv->release = shader_arena_blob_release;
  spirv->code = reinterpret_cast<const u8*>(result.spirv);
  spirv->size = result.spirv_len * sizeof(uint32_t);
  return true;
}
}  // namespace Vulkan::ShaderCompiler
