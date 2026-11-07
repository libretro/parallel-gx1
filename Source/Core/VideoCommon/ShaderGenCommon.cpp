// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderGenCommon.h"

#include <stdarg.h>
#include <stdio.h>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

void ShaderCode::Printf(const char* format, ...)
{
  char line[512];
  va_list ap;
  int n;

  /* a line formats on the stack; longer goes through the text's own */
  va_start(ap, format);
  n = vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);
  if (n >= 0 && static_cast<size_t>(n) < sizeof(line))
  {
    shader_text_putn(&m_text, line, static_cast<size_t>(n));
    return;
  }
  va_start(ap, format);
  n = vsnprintf(nullptr, 0, format, ap);
  va_end(ap);
  if (n < 0)
    return;
  {
    char* const big = static_cast<char*>(malloc(static_cast<size_t>(n) + 1));
    if (!big)
      return;
    va_start(ap, format);
    vsnprintf(big, static_cast<size_t>(n) + 1, format, ap);
    va_end(ap);
    shader_text_putn(&m_text, big, static_cast<size_t>(n));
    free(big);
  }
}

void WriteSwitchTree(ShaderCode& out, const char* variable, const char* const* values,
                     const char* const* names, u32 indent, u32 low, u32 high)
{
  u32 mid;

  // Each generated statement is for low <= x < high
  if (high == low + 1)
  {
    // Down to 1 case (low <= x < low + 1 means x == low)
    // Note that this indentation behaves poorly for multi-line code
    out.Indent(indent);
    out.Printf("%s  // %s (%u)\n", values[low], names[low], low);
    return;
  }
  mid = low + ((high - low) / 2);
  out.Indent(indent);
  out.Printf("if (%s < %uu) {\n", variable, mid);
  WriteSwitchTree(out, variable, values, names, indent + 2, low, mid);
  out.Indent(indent);
  out.Puts("} else {\n");
  WriteSwitchTree(out, variable, values, names, indent + 2, mid, high);
  out.Indent(indent);
  out.Puts("}\n");
}

ShaderHostConfig ShaderHostConfig::GetCurrent()
{
  ShaderHostConfig bits = {};
  bits.msaa = g_ActiveConfig.iMultisamples > 1;
  bits.ssaa =
      g_ActiveConfig.iMultisamples > 1 && g_ActiveConfig.bSSAA && g_backend_info.bSupportsSSAA;
  bits.stereo = g_ActiveConfig.stereo_mode != StereoMode::Off;
  bits.wireframe = g_ActiveConfig.bWireFrame;
  bits.per_pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  bits.vertex_rounding = g_ActiveConfig.UseVertexRounding();
  bits.fast_depth_calc = g_ActiveConfig.bFastDepthCalc;
  bits.bounding_box = g_ActiveConfig.bBBoxEnable;
  bits.backend_dual_source_blend = g_backend_info.bSupportsDualSourceBlend;
  bits.backend_geometry_shaders = g_backend_info.bSupportsGeometryShaders;
  bits.backend_early_z = g_backend_info.bSupportsEarlyZ;
  bits.backend_bbox = g_backend_info.bSupportsBBox;
  bits.backend_gs_instancing = g_backend_info.bSupportsGSInstancing;
  bits.backend_clip_control = g_backend_info.bSupportsClipControl;
  bits.backend_ssaa = g_backend_info.bSupportsSSAA;
  bits.backend_atomics = g_backend_info.bSupportsFragmentStoresAndAtomics;
  bits.backend_depth_clamp = g_backend_info.bSupportsDepthClamp;
  bits.backend_reversed_depth_range = g_backend_info.bSupportsReversedDepthRange;
  bits.backend_bitfield = g_backend_info.bSupportsBitfield;
  bits.backend_dynamic_sampler_indexing = g_backend_info.bSupportsDynamicSamplerIndexing;
  bits.backend_shader_framebuffer_fetch = g_backend_info.bSupportsFramebufferFetch;
  bits.backend_logic_op = g_backend_info.bSupportsLogicOp;
  bits.backend_palette_conversion = g_backend_info.bSupportsPaletteConversion;
  bits.enable_validation_layer = g_ActiveConfig.bEnableValidationLayer;
  bits.manual_texture_sampling = !g_ActiveConfig.bFastTextureSampling;
  bits.manual_texture_sampling_custom_texture_sizes =
      g_ActiveConfig.ManualTextureSamplingWithCustomTextureSizes();
  bits.backend_sampler_lod_bias = g_backend_info.bSupportsLodBiasInSampler;
  bits.backend_dynamic_vertex_loader = g_backend_info.bSupportsDynamicVertexLoader;
  bits.backend_vs_point_line_expand = g_ActiveConfig.UseVSForLinePointExpand();
  bits.backend_gl_layer_in_fs = g_backend_info.bSupportsGLLayerInFS;
  return bits;
}

std::string GetDiskShaderCacheFileName(APIType api_type, const char* type, bool include_gameid,
                                       bool include_host_config, bool include_api)
{
  if (!File::Exists(File::GetUserPath(D_SHADERCACHE_IDX)))
    File::CreateDir(File::GetUserPath(D_SHADERCACHE_IDX));

  std::string filename = File::GetUserPath(D_SHADERCACHE_IDX);
  if (include_api)
  {
    switch (api_type)
    {
    case APIType::D3D:
      filename += "D3D";
      break;
    case APIType::Metal:
      filename += "Metal";
      break;
    case APIType::OpenGL:
      filename += "OpenGL";
      break;
    case APIType::Vulkan:
      filename += "Vulkan";
      break;
    default:
      break;
    }
    filename += '-';
  }

  filename += type;

  if (include_gameid)
  {
    filename += '-';
    filename += SConfig::GetInstance().GetGameID();
  }

  if (include_host_config)
  {
    // We're using 21 bits, so 6 hex characters.
    ShaderHostConfig host_config = ShaderHostConfig::GetCurrent();
    filename += fmt::format("-{:06X}", host_config.bits);

#if defined(__WEBOS__)
    // The webOS core option toggles binding layout, which changes the
    // generated GLSL
    if (g_backend_info.bSupportsBindingLayout)
      filename += "-bl";
#endif
  }

  filename += ".cache";
  return filename;
}

void WriteIsNanHeader(ShaderCode& out, APIType api_type)
{
  if (api_type == APIType::D3D)
  {
    out.Puts("bool dolphin_isnan(float f) {\n"
              "  // Workaround for the HLSL compiler deciding that isnan can never be true and\n"
              "  // optimising away the call, even though the value can actually be NaN\n"
              "  // Just look for the bit pattern that indicates NaN instead\n"
              "  return (floatBitsToInt(f) & 0x7FFFFFFF) > 0x7F800000;\n"
              "}\n"
              "\n");
    // If isfinite is needed, (floatBitsToInt(f) & 0x7F800000) != 0x7F800000 can be used
  }
  else
  {
    out.Puts("#define dolphin_isnan(f) isnan(f)\n");
  }
}

void WriteBitfieldExtractHeader(ShaderCode& out, APIType api_type,
                                const ShaderHostConfig& host_config)
{
  // ==============================================
  //  BitfieldExtract for APIs which don't have it
  // ==============================================
  if (!host_config.backend_bitfield)
  {
    out.Puts("uint bitfieldExtract(uint val, int off, int size) {\n"
              "  // This built-in function is only supported in OpenGL 4.0+ and ES 3.1+\n"
              "  // Microsoft's HLSL compiler automatically optimises this to a bitfield extract instruction.\n"
              "  uint mask = uint((1 << size) - 1);\n"
              "  return uint(val >> off) & mask;\n"
              "}\n"
              "\n");
    out.Puts("int bitfieldExtract(int val, int off, int size) {\n"
              "  // This built-in function is only supported in OpenGL 4.0+ and ES 3.1+\n"
              "  // Microsoft's HLSL compiler automatically optimises this to a bitfield extract instruction.\n"
              "  return ((val << (32 - size - off)) >> (32 - size));\n"
              "}\n"
              "\n");
  }
}

static void DefineOutputMember(ShaderCode& object, const char* qualifier,
                               const char* type, const char* name, int var_index)
{
  object.Printf("\t%s %s %s", qualifier, type, name);

  if (var_index != -1)
    object.Printf("%d", var_index);

  object.Puts(";\n");
}

void GenerateVSOutputMembers(ShaderCode& object, u32 texgens, const ShaderHostConfig& host_config,
                             const char* qualifier)
{
  DefineOutputMember(object, qualifier, "float4", "pos", -1);
  DefineOutputMember(object, qualifier, "float4", "colors_", 0);
  DefineOutputMember(object, qualifier, "float4", "colors_", 1);

  if (host_config.backend_geometry_shaders)
  {
    DefineOutputMember(object, qualifier, "float", "clipDist", 0);
    DefineOutputMember(object, qualifier, "float", "clipDist", 1);
  }

  for (unsigned int i = 0; i < texgens; ++i)
    DefineOutputMember(object, qualifier, "float3", "tex", i);

  if (!host_config.fast_depth_calc)
    DefineOutputMember(object, qualifier, "float4", "clipPos", -1);

  if (host_config.per_pixel_lighting)
  {
    DefineOutputMember(object, qualifier, "float3", "Normal", -1);
    DefineOutputMember(object, qualifier, "float3", "WorldPos", -1);
  }
}

void AssignVSOutputMembers(ShaderCode& object, const char* a, const char* b, u32 texgens,
                           const ShaderHostConfig& host_config)
{
  object.Printf("\t%s.pos = %s.pos;\n", a, b);
  object.Printf("\t%s.colors_0 = %s.colors_0;\n", a, b);
  object.Printf("\t%s.colors_1 = %s.colors_1;\n", a, b);

  for (unsigned int i = 0; i < texgens; ++i)
    object.Printf("\t%s.tex%u = %s.tex%u;\n", a, i, b, i);

  if (!host_config.fast_depth_calc)
    object.Printf("\t%s.clipPos = %s.clipPos;\n", a, b);

  if (host_config.per_pixel_lighting)
  {
    object.Printf("\t%s.Normal = %s.Normal;\n", a, b);
    object.Printf("\t%s.WorldPos = %s.WorldPos;\n", a, b);
  }

  if (host_config.backend_geometry_shaders)
  {
    object.Printf("\t%s.clipDist0 = %s.clipDist0;\n", a, b);
    object.Printf("\t%s.clipDist1 = %s.clipDist1;\n", a, b);
  }
}

void GenerateLineOffset(ShaderCode& object, const char* indent0, const char* indent1,
                        const char* pos_a, const char* pos_b, const char* sign)
{
  // GameCube/Wii's line drawing algorithm is a little quirky. It does not
  // use the correct line caps. Instead, the line caps are vertical or
  // horizontal depending the slope of the line.
  object.Printf("%sfloat2 offset;\n", indent0);
  object.Printf("%sfloat2 to = abs(%s.xy / %s.w - %s.xy / %s.w);\n", indent0, pos_a, pos_a, pos_b,
                pos_b);
  // FIXME: What does real hardware do when line is at a 45-degree angle?
  // FIXME: Lines aren't drawn at the correct width. See Twilight Princess map.
  object.Printf("%sif (" I_LINEPTPARAMS ".y * to.y > " I_LINEPTPARAMS ".x * to.x) {\n", indent0);
  // Line is more tall. Extend geometry left and right.
  // Lerp LineWidth/2 from [0..VpWidth] to [-1..1]
  object.Printf("%soffset = float2(%s" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".x, 0);\n", indent1,
                sign);
  object.Printf("%s} else {\n", indent0);
  // Line is more wide. Extend geometry up and down.
  // Lerp LineWidth/2 from [0..VpHeight] to [1..-1]
  object.Printf("%soffset = float2(0, %s-" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".y);\n", indent1,
                sign);
  object.Printf("%s}\n", indent0);
}

void GenerateVSLineExpansion(ShaderCode& object, const char* indent, u32 texgens)
{
  char indent1[64];
  snprintf(indent1, sizeof(indent1), "%s  ", indent);
  object.Printf("%sother_pos = float4(dot("
            I_PROJECTION
            "[0], other_pos), dot("
            I_PROJECTION
            "[1], other_pos), dot("
            I_PROJECTION
            "[2], other_pos), dot("
            I_PROJECTION
            "[3], other_pos));\n"
            "\n"
            "%sfloat expand_sign = is_right ? 1.0f : -1.0f;\n", indent, indent);
  GenerateLineOffset(object, indent, indent1, "o.pos", "other_pos", "expand_sign * ");
  object.Printf("\n"
            "%so.pos.xy += offset * o.pos.w;\n", indent);
  if (texgens > 0)
  {
    object.Printf("%sif (("
              I_TEXOFFSET
              "[2] != 0) && is_right) {\n", indent);
    object.Printf("%s  float texOffset = 1.0 / float("
              I_TEXOFFSET
              "[2]);\n", indent);
    for (u32 i = 0; i < texgens; i++)
    {
      object.Printf("%s  if ((("
                I_TEXOFFSET
                "[0] >> %u) & 0x1) != 0)\n", indent, i);
      object.Printf("%s    o.tex%u.x += texOffset;\n", indent, i);
    }
    object.Printf("%s}\n", indent);
  }
}

void GenerateVSPointExpansion(ShaderCode& object, const char* indent, u32 texgens)
{
  object.Printf("%sfloat2 expand_sign = float2(is_right ? 1.0f : -1.0f, is_bottom ? -1.0f : 1.0f);\n"
            "%sfloat2 offset = expand_sign * "
            I_LINEPTPARAMS
            ".ww / "
            I_LINEPTPARAMS
            ".xy;\n"
            "%so.pos.xy += offset * o.pos.w;\n", indent, indent, indent);
  if (texgens > 0)
  {
    object.Printf("%sif ("
              I_TEXOFFSET
              "[3] != 0) {\n"
              "%s  float texOffsetMagnitude = 1.0f / float("
              I_TEXOFFSET
              "[3]);\n"
              "%s  float2 texOffset = float2(is_right ? texOffsetMagnitude : 0.0f, is_bottom ? texOffsetMagnitude : 0.0f);", indent, indent, indent);
    for (u32 i = 0; i < texgens; i++)
    {
      object.Printf("%s  if ((("
                I_TEXOFFSET
                "[1] >> %u) & 0x1) != 0)\n", indent, i);
      object.Printf("%s    o.tex%u.xy += texOffset;\n", indent, i);
    }
    object.Printf("%s}\n", indent);
  }
}

const char* GetInterpolationQualifier(bool msaa, bool ssaa, bool in_glsl_interface_block, bool in)
{
  if (!msaa)
    return "";

  // Without GL_ARB_shading_language_420pack support, the interpolation qualifier must be
  // "centroid in" and not "centroid", even within an interface block.
  if (in_glsl_interface_block && !g_backend_info.bSupportsBindingLayout)
  {
    if (!ssaa)
      return in ? "centroid in" : "centroid out";
    else
      return in ? "sample in" : "sample out";
  }
  else
  {
    if (!ssaa)
      return "centroid";
    else
      return "sample";
  }
}
