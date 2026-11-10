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

/* Draws one pixel over px_in with the library's fs_triangle and the
 * named fragment function and reads it back to px_out, for the probes of
 * driver behaviour. False when the library, the pipeline or the draw
 * fails. */
static bool RenderSinglePixel(id<MTLDevice> dev, const char* shader, NSString* fs_name, u32 px_in,
                              u32* px_out)
{
  bool ok = false;
  id<MTLLibrary> lib;
  id<MTLFunction> vs;
  id<MTLFunction> fs;
  MTLRenderPipelineDescriptor* pdesc;
  id<MTLRenderPipelineState> pipe = nil;
  id<MTLBuffer> buf = nil;
  MTLTextureDescriptor* tdesc;
  id<MTLTexture> tex = nil;
  id<MTLCommandQueue> q = nil;
  id<MTLCommandBuffer> cmdbuf;
  id<MTLBlitCommandEncoder> blit;
  MTLRenderPassDescriptor* rpdesc;
  id<MTLRenderCommandEncoder> renc;

  lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:shader] options:nil error:nil];
  if (!lib)
    return false;
  vs = [lib newFunctionWithName:@"fs_triangle"];
  fs = [lib newFunctionWithName:fs_name];
  pdesc = [MTLRenderPipelineDescriptor new];
  [pdesc setVertexFunction:vs];
  [pdesc setFragmentFunction:fs];
  [[pdesc colorAttachments][0] setPixelFormat:MTLPixelFormatRGBA8Unorm];
  if (vs && fs)
    pipe = [dev newRenderPipelineStateWithDescriptor:pdesc error:nil];
  if (pipe)
  {
    buf = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    memcpy([buf contents], &px_in, sizeof(px_in));
    tdesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1
                                                              height:1
                                                           mipmapped:false];
    [tdesc setUsage:MTLTextureUsageRenderTarget];
    tex = [dev newTextureWithDescriptor:tdesc];
    q = [dev newCommandQueue];
    cmdbuf = [q commandBuffer];

    blit = [cmdbuf blitCommandEncoder];
    [blit copyFromBuffer:buf
               sourceOffset:0
          sourceBytesPerRow:4
        sourceBytesPerImage:4
                 sourceSize:MTLSizeMake(1, 1, 1)
                  toTexture:tex
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];

    rpdesc = [MTLRenderPassDescriptor new];
    [[rpdesc colorAttachments][0] setTexture:tex];
    [[rpdesc colorAttachments][0] setLoadAction:MTLLoadActionLoad];
    [[rpdesc colorAttachments][0] setStoreAction:MTLStoreActionStore];
    renc = [cmdbuf renderCommandEncoderWithDescriptor:rpdesc];
    [renc setRenderPipelineState:pipe];
    [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [renc endEncoding];
    [rpdesc release];

    blit = [cmdbuf blitCommandEncoder];
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:4
        destinationBytesPerImage:4];
    [blit endEncoding];

    [cmdbuf commit];
    [cmdbuf waitUntilCompleted];

    memcpy(px_out, [buf contents], sizeof(*px_out));
    ok = [cmdbuf status] == MTLCommandBufferStatusCompleted;
  }

  [q release];
  [tex release];
  [buf release];
  [pipe release];
  [pdesc release];
  [fs release];
  [vs release];
  [lib release];
  return ok;
}

#define PROBE_TRIANGLE                                                                         \
  "vertex float4 fs_triangle(uint vid [[vertex_id]]) {\n"                                       \
  "  return float4(vid & 1 ? 3 : -1, vid & 2 ? 3 : -1, 0, 1);\n"                                 \
  "}\n"

#if TARGET_OS_OSX
/* Some Intel GPUs fetch from the framebuffer though no feature set says
 * so, and others build the pipeline but leave the fetch out: the pixel
 * doubles what it reads, which Skylake and later do; Broadwell leaves
 * it and Haswell blackens it. */
static bool DetectIntelGPUFBFetch(id<MTLDevice> dev)
{
  static const char shader[] =
      PROBE_TRIANGLE
      "fragment float4 fbfetch_test(float4 in [[color(0), raster_order_group(0)]]) {\n"
      "  return in * 2;\n"
      "}\n";
  u32 px = 0;
  return RenderSinglePixel(dev, shader, @"fbfetch_test", 0x11223344, &px) && px == 0x22446688;
}
#endif

/* Whether simd_is_helper_thread() answers inverted: the drawn pixel is
 * no helper (0.5) and its right and lower neighbours are (1.0), so a
 * working driver gives 0.5 and derivatives of 0.5 + 0.5, an inverted one
 * 1.0 and derivatives of -0.5 + 0.5. 1 inverted, 0 working, -1 unsure. */
static int DetectInvertedIsHelper(id<MTLDevice> dev)
{
  static const char shader[] =
      PROBE_TRIANGLE
      "fragment float4 is_helper_test() {\n"
      "  float val = metal::simd_is_helper_thread() ? 1 : 0.5;\n"
      "  return float4(val, metal::dfdx(val) + 0.5, metal::dfdy(val) + 0.5, 0);\n"
      "}\n";
  u32 px = 0;
  if (!RenderSinglePixel(dev, shader, @"is_helper_test", 0, &px))
    return -1;
  if (px == 0xffff80)
    return 0;
  if (px == 0x0000ff)
    return 1;
  WARN_LOG_FMT(VIDEO, "metal::simd_is_helper_thread might be broken! Test shader returned {:06x}!",
               px);
  return -1;
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

  g_features.subgroup_ops =
      [device supportsFamily:MTLGPUFamilyMac2] || [device supportsFamily:MTLGPUFamilyApple7];
  if (g_features.subgroup_ops)
  {
    int inverted = DetectInvertedIsHelper(device);
    if (inverted >= 0 &&
        (inverted != 0) != DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER))
      DriverDetails::OverrideBug(DriverDetails::BUG_INVERTED_IS_HELPER, inverted != 0);
  }

  backend_info->bSupportsFramebufferFetch = [device supportsFamily:MTLGPUFamilyApple1];
#if TARGET_OS_OSX
  if (vendor == DriverDetails::VENDOR_INTEL)
    backend_info->bSupportsFramebufferFetch |= DetectIntelGPUFBFetch(device);
#endif

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
#define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)

// Metal framebuffer fetch helpers.
#define FB_FETCH_VALUE subpassLoad(in_ocol0)

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

/* The bounding box's subgroup reduction, where the GPU has simdgroup
 * functions; rslc prints it with them. */
static const std::string_view SUBGROUP_HELPER_HEADER = R"(
#extension GL_KHR_shader_subgroup_basic : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable
#define SUPPORTS_SUBGROUP_REDUCTION 1
#define IS_HELPER_INVOCATION gl_HelperInvocation
#define IS_FIRST_ACTIVE_INVOCATION (subgroupElect())
#define SUBGROUP_MIN(value) value = subgroupMin(value)
#define SUBGROUP_MAX(value) value = subgroupMax(value)
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
  if (Metal::g_features.subgroup_ops)
    full_source.append(SUBGROUP_HELPER_HEADER);
  if (DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER))
  {
    full_source.append("#define gl_HelperInvocation !gl_HelperInvocation "
                       "// Work around broken AMD Metal driver\n");
  }
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD))
    full_source.append("#define BROKEN_SUBGROUP_WITH_DISCARD 1\n");
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
  return FinishMSL(compiled);
}

std::string Metal::Util::FinishMSL(std::string_view compiled)
{
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
