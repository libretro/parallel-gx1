// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Metal/MTLUtil.h"

#include <string>

#include <cstdio>
#include <cstring>

#include <TargetConditionals.h>
#include <gfx/shaders/rslc.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/VideoBackendBase.h"

Metal::DeviceFeatures Metal::g_features;

std::vector<MRCOwned<id<MTLDevice>>> Metal::Util::GetAdapterList()
{
  std::vector<MRCOwned<id<MTLDevice>>> list;
  id<MTLDevice> default_dev = MTLCreateSystemDefaultDevice();
  if (default_dev)
    list.push_back(MRCTransfer(default_dev));

#if TARGET_OS_OSX
  auto devices = MRCTransfer(MTLCopyAllDevices());
  for (id<MTLDevice> device in devices.Get())
  {
    if (device != default_dev)
      list.push_back(MRCRetain(device));
  }
#endif

  return list;
}

void Metal::Util::PopulateBackendInfo(BackendInfo* backend_info)
{
  backend_info->api_type = APIType::Metal;
  backend_info->bUsesLowerLeftOrigin = false;
  backend_info->bSupportsExclusiveFullscreen = false;
  backend_info->bSupportsDualSourceBlend = true;
  backend_info->bSupportsPrimitiveRestart = true;
  backend_info->bSupportsGeometryShaders = false;
  backend_info->bSupportsComputeShaders = true;
  backend_info->bSupports3DVision = false;
  backend_info->bSupportsEarlyZ = true;
  backend_info->bSupportsBindingLayout = true;
  backend_info->bSupportsBBox = true;
  backend_info->bSupportsGSInstancing = false;
  backend_info->bSupportsPostProcessing = true;
  backend_info->bSupportsPaletteConversion = true;
  backend_info->bSupportsClipControl = true;
  backend_info->bSupportsSSAA = true;
  backend_info->bSupportsFragmentStoresAndAtomics = true;
  backend_info->bSupportsReversedDepthRange = false;
  backend_info->bSupportsLogicOp = false;
  backend_info->bSupportsMultithreading = false;
  backend_info->bSupportsGPUTextureDecoding = true;
  backend_info->bSupportsCopyToVram = true;
  backend_info->bSupportsBitfield = true;
  backend_info->bSupportsDynamicSamplerIndexing = true;
  backend_info->bSupportsFramebufferFetch = false;
  backend_info->bSupportsBackgroundCompiling = true;
  backend_info->bSupportsLargePoints = true;
  backend_info->bSupportsPartialDepthCopies = true;
  backend_info->bSupportsDepthReadback = true;
  backend_info->bSupportsShaderBinaries = false;
  backend_info->bSupportsPipelineCacheData = false;
  backend_info->bSupportsCoarseDerivatives = false;
  backend_info->bSupportsTextureQueryLevels = true;
  backend_info->bSupportsLodBiasInSampler = false;
  backend_info->bSupportsSettingObjectNames = true;
  // Metal requires multisample resolve to be done on a render pass
  backend_info->bSupportsPartialMultisampleResolve = false;
  backend_info->bSupportsDynamicVertexLoader = true;
  backend_info->bSupportsVSLinePointExpand = true;
  backend_info->bSupportsHDROutput =
      1.0 < [[NSScreen deepestScreen] maximumPotentialExtendedDynamicRangeColorComponentValue];
}

void Metal::Util::PopulateBackendInfoAdapters(BackendInfo* backend_info,
                                              const std::vector<MRCOwned<id<MTLDevice>>>& adapters)
{
  backend_info->Adapters.clear();
  for (id<MTLDevice> adapter : adapters)
  {
    backend_info->Adapters.push_back([[adapter name] UTF8String]);
  }
}

void Metal::Util::PopulateBackendInfoFeatures(const VideoConfig& config, BackendInfo* backend_info,
                                              id<MTLDevice> device)
{
  // Initialize DriverDetails first so we can use it later
  DriverDetails::Vendor vendor = DriverDetails::VENDOR_UNKNOWN;
  std::string name = [[device name] UTF8String];
  if (name.contains("NVIDIA"))
    vendor = DriverDetails::VENDOR_NVIDIA;
  else if (name.contains("AMD"))
    vendor = DriverDetails::VENDOR_ATI;
  else if (name.contains("Intel"))
    vendor = DriverDetails::VENDOR_INTEL;
  else if (name.contains("Apple"))
    vendor = DriverDetails::VENDOR_APPLE;
  DriverDetails::Init(DriverDetails::API_METAL, vendor, DriverDetails::DRIVER_APPLE, 0.0,
                      DriverDetails::Family::UNKNOWN, std::move(name));

#if TARGET_OS_OSX
  backend_info->bSupportsDepthClamp = true;
  backend_info->bSupportsST3CTextures = true;
  backend_info->bSupportsBPTCTextures = true;
#else
  backend_info->bSupportsDepthClamp = [device supportsFamily:MTLGPUFamilyApple4];

  bool supports_bcn = false;
  if (@available(iOS 16.4, *))
    supports_bcn = [device supportsBCTextureCompression];
  backend_info->bSupportsST3CTextures = supports_bcn;
  backend_info->bSupportsBPTCTextures = supports_bcn;
#endif

  backend_info->AAModes.clear();
  for (u32 i = 1; i <= 64; i <<= 1)
  {
    if ([device supportsTextureSampleCount:i])
      backend_info->AAModes.push_back(i);
  }

  switch (config.iManuallyUploadBuffers)
  {
  case TriState::Off:
    g_features.manual_buffer_upload = false;
    break;
  case TriState::On:
    g_features.manual_buffer_upload = true;
    break;
  case TriState::Auto:
#if TARGET_OS_OSX
    g_features.manual_buffer_upload = false;
    if (![device hasUnifiedMemory])
      g_features.manual_buffer_upload = true;
#else
    // All iOS devices have unified memory
    g_features.manual_buffer_upload = false;
#endif
    break;
  }

  /* rslc has no form of a subpass input: blending is not emulated
   * through framebuffer fetch */
  backend_info->bSupportsFramebufferFetch = false;

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DYNAMIC_SAMPLER_INDEXING))
    backend_info->bSupportsDynamicSamplerIndexing = false;
}

// clang-format off

AbstractTextureFormat Metal::Util::ToAbstract(MTLPixelFormat format)
{
  switch (format)
  {
  case MTLPixelFormatRGBA8Unorm:            return AbstractTextureFormat::RGBA8;
  case MTLPixelFormatBGRA8Unorm:            return AbstractTextureFormat::BGRA8;
  case MTLPixelFormatRGB10A2Unorm:          return AbstractTextureFormat::RGB10_A2;
  case MTLPixelFormatRGBA16Float:           return AbstractTextureFormat::RGBA16F;
  case MTLPixelFormatBC1_RGBA:              return AbstractTextureFormat::DXT1;
  case MTLPixelFormatBC2_RGBA:              return AbstractTextureFormat::DXT3;
  case MTLPixelFormatBC3_RGBA:              return AbstractTextureFormat::DXT5;
  case MTLPixelFormatBC7_RGBAUnorm:         return AbstractTextureFormat::BPTC;
  case MTLPixelFormatR16Unorm:              return AbstractTextureFormat::R16;
  case MTLPixelFormatDepth16Unorm:          return AbstractTextureFormat::D16;
#if TARGET_OS_OSX
  case MTLPixelFormatDepth24Unorm_Stencil8: return AbstractTextureFormat::D24_S8;
#endif
  case MTLPixelFormatR32Float:              return AbstractTextureFormat::R32F;
  case MTLPixelFormatDepth32Float:          return AbstractTextureFormat::D32F;
  case MTLPixelFormatDepth32Float_Stencil8: return AbstractTextureFormat::D32F_S8;
  default:                                  return AbstractTextureFormat::Undefined;
  }
}

// Don't complain about BCn formats requiring iOS 16.4, these are just enum conversions
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"

MTLPixelFormat Metal::Util::FromAbstract(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:     return MTLPixelFormatRGBA8Unorm;
  case AbstractTextureFormat::BGRA8:     return MTLPixelFormatBGRA8Unorm;
  case AbstractTextureFormat::RGB10_A2:  return MTLPixelFormatRGB10A2Unorm;
  case AbstractTextureFormat::RGBA16F:   return MTLPixelFormatRGBA16Float;
  case AbstractTextureFormat::DXT1:      return MTLPixelFormatBC1_RGBA;
  case AbstractTextureFormat::DXT3:      return MTLPixelFormatBC2_RGBA;
  case AbstractTextureFormat::DXT5:      return MTLPixelFormatBC3_RGBA;
  case AbstractTextureFormat::BPTC:      return MTLPixelFormatBC7_RGBAUnorm;
  case AbstractTextureFormat::R16:       return MTLPixelFormatR16Unorm;
  case AbstractTextureFormat::D16:       return MTLPixelFormatDepth16Unorm;
#if TARGET_OS_OSX
  case AbstractTextureFormat::D24_S8:    return MTLPixelFormatDepth24Unorm_Stencil8;
#endif
  case AbstractTextureFormat::R32F:      return MTLPixelFormatR32Float;
  case AbstractTextureFormat::D32F:      return MTLPixelFormatDepth32Float;
  case AbstractTextureFormat::D32F_S8:   return MTLPixelFormatDepth32Float_Stencil8;
  default:                               return MTLPixelFormatInvalid;
  }
}

#pragma clang diagnostic pop

// clang-format on

// MARK: Shader Translation

/* The stages' headers. A binding is the Metal index the backend binds
 * the resource at, as rslc prints it: the vertex stage's uniform blocks
 * at buffers 1 and 2 (the expand block), its vertex loader input at
 * buffer 0; the pixel stage's blocks at buffers 0 and 1, the bounding
 * box at buffer 2; textures and samplers at their own index; compute's
 * block at buffer 0, its buffers at 2 and 3, its images after the eight
 * samplers. */
static const std::string_view VERTEX_SHADER_HEADER = R"(
#version 450 core
#define ATTRIBUTE_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
#define SAMPLER_BINDING(x) layout(binding = x)
#define TEXEL_BUFFER_BINDING(x) layout(binding = (x + 8))
#define VARYING_LOCATION(x) layout(location = x)
#define FORCE_EARLY_Z layout(early_fragment_tests) in
#define UBO_BINDING(packing, x) layout(packing, binding = (1 + (x) / 4))
#define SSBO_BINDING(x) layout(std430, binding = ((x) - 1))

// hlsl to glsl function translation
#define API_METAL 1
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
static const std::string_view PIXEL_SHADER_HEADER = R"(
#version 450 core
#define ATTRIBUTE_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
#define SAMPLER_BINDING(x) layout(binding = x)
#define TEXEL_BUFFER_BINDING(x) layout(binding = (x + 8))
#define VARYING_LOCATION(x) layout(location = x)
#define FORCE_EARLY_Z layout(early_fragment_tests) in
#define UBO_BINDING(packing, x) layout(packing, binding = ((x) - 1))
#define SSBO_BINDING(x) layout(std430, binding = ((x) + 2))

// hlsl to glsl function translation
#define API_METAL 1
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
static const std::string_view COMPUTE_SHADER_HEADER = R"(
#version 450 core
#define UBO_BINDING(packing, x) layout(packing, binding = ((x) - 1))
#define SAMPLER_BINDING(x) layout(binding = x)
#define SSBO_BINDING(x) layout(std430, binding = ((x) + 2))
#define IMAGE_BINDING(format, x) layout(format, binding = (8 + (x)))

// hlsl to glsl function translation
#define API_METAL 1
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

static const std::string_view MSL_HEADER =
    // We know our shader generator leaves unused variables.
    "#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
    // These are usually when the compiler doesn't think a switch is exhaustive
    "#pragma clang diagnostic ignored \"-Wreturn-type\"\n";

static constexpr std::pair<std::string_view, std::string_view> MSL_FIXUPS[] = {
    // Force-unroll the lighting loop in ubershaders, which greatly reduces register pressure on AMD
    {"for (uint chan = 0u; chan < 2u; chan++)",
     "_Pragma(\"unroll\") for (uint chan = 0u; chan < 2u; chan++)"},
};

std::optional<std::string>
Metal::Util::TranslateShaderToMSL(ShaderStage stage, std::string_view source)
{
  static int num_failures;
  char err[1024];
  struct rslc_opts opts;
  struct rslc_result result;
  enum rslc_stage rstage;
  std::string full_source;
  std::string compiled;
  FILE* f;

  switch (stage)
  {
  case ShaderStage::Vertex:
    rstage = RSLC_STAGE_VERTEX;
    full_source.append(VERTEX_SHADER_HEADER);
    break;
  case ShaderStage::Pixel:
    rstage = RSLC_STAGE_FRAGMENT;
    full_source.append(PIXEL_SHADER_HEADER);
    break;
  case ShaderStage::Compute:
    rstage = RSLC_STAGE_COMPUTE;
    full_source.append(COMPUTE_SHADER_HEADER);
    break;
  default:
    PanicAlertFmt("Tried to compile geometry shader for Metal, but Metal doesn't support them!");
    return std::nullopt;
  }
  full_source.append(source);

  memset(&opts, 0, sizeof(opts));
  opts.dst = RSLC_DST_MSL;
  opts.version = 23;   /* MSL 2.3 */
  opts.err = err;
  opts.err_cap = sizeof(err);
  err[0] = '\0';
  if (!rslc_compile(full_source.data(), full_source.size(), rstage, &opts, &result))
  {
    std::string path = VideoBackendBase::BadShaderFilename("rslc", num_failures++);
    ERROR_LOG_FMT(VIDEO, "rslc could not print the MSL of a shader: {}", err);
    File::CreateFullPath(path);
    f = fopen(path.c_str(), "wb");
    if (f)
    {
      fwrite(full_source.data(), 1, full_source.size(), f);
      fprintf(f, "\n/* rslc: %s */\n", err);
      fclose(f);
    }
    return std::nullopt;
  }
  compiled = result.text;
  rslc_result_free(&result, &opts);

  std::string output(MSL_HEADER);
  std::string_view remaining = compiled;
  while (!remaining.empty())
  {
    // Apply fixups
    std::string_view piece = remaining;
    std::string_view fixup_piece = {};
    size_t next = piece.size();
    for (const auto& fixup : MSL_FIXUPS)
    {
      size_t found = piece.find(fixup.first);
      if (found == std::string_view::npos)
        continue;
      piece = piece.substr(0, found);
      fixup_piece = fixup.second;
      next = found + fixup.first.size();
    }
    output += piece;
    output += fixup_piece;
    remaining = remaining.substr(next);
  }
  return output;
}
