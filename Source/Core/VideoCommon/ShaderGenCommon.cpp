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

uint32_t sb_place_ref(const struct shader_build* sb, const struct sb_place* p)
{
  uint32_t e = sb_ref(sb, p->var);
  if (p->index_var)
    return sb_idx(sb, e, sb_ref(sb, p->index_var));
  if (p->index >= 0)
    return sb_idx(sb, e, sb_i(sb, p->index));
  return e;
}

void BuildVSOutputLayout(const struct shader_build* sb, u32 texgens,
                         const ShaderHostConfig& host_config, unsigned quals,
                         struct vs_output_layout* l)
{
  static const char* const tex_names[8] = {"tex0", "tex1", "tex2", "tex3",
                                           "tex4", "tex5", "tex6", "tex7"};
  u32 n = 0, i;
#define VS_OUT_MEMBER(idx, type, name)                                                            \
  do                                                                                               \
  {                                                                                                \
    l->idx = static_cast<int>(n);                                                                 \
    l->types[n] = (type);                                                                          \
    l->names[n] = (name);                                                                          \
    l->quals[n++] = quals;                                                                         \
  } while (0)
  l->clipDist0 = l->clipDist1 = l->clipPos = l->Normal = l->WorldPos = -1;
  l->texgens = texgens;
  VS_OUT_MEMBER(pos, sb->f[4], "pos");
  VS_OUT_MEMBER(colors_0, sb->f[4], "colors_0");
  VS_OUT_MEMBER(colors_1, sb->f[4], "colors_1");
  if (host_config.backend_geometry_shaders)
  {
    VS_OUT_MEMBER(clipDist0, sb->f[1], "clipDist0");
    VS_OUT_MEMBER(clipDist1, sb->f[1], "clipDist1");
  }
  l->tex0 = static_cast<int>(n);
  for (i = 0; i < texgens; i++)
  {
    l->types[n] = sb->f[3];
    l->names[n] = tex_names[i];
    l->quals[n++] = quals;
  }
  if (!host_config.fast_depth_calc)
    VS_OUT_MEMBER(clipPos, sb->f[4], "clipPos");
  if (host_config.per_pixel_lighting)
  {
    VS_OUT_MEMBER(Normal, sb->f[3], "Normal");
    VS_OUT_MEMBER(WorldPos, sb->f[3], "WorldPos");
  }
#undef VS_OUT_MEMBER
  l->n = n;
}

unsigned BuildInterpolationQuals(bool msaa, bool ssaa)
{
  if (!msaa)
    return 0;
  return ssaa ? RSLC_B_SAMPLE : RSLC_B_CENTROID;
}

void BuildAssignVSOutputMembers(const struct shader_build* sb, const struct vs_output_layout* l,
                                const struct sb_place* a, const struct sb_place* b,
                                const ShaderHostConfig& host_config)
{
  u32 i;
#define VS_OUT_ASSIGN(idx)                                                                        \
  sb_set(sb, sb_mem(sb, sb_place_ref(sb, a), static_cast<uint32_t>(idx)),                         \
         sb_mem(sb, sb_place_ref(sb, b), static_cast<uint32_t>(idx)))
  VS_OUT_ASSIGN(l->pos);
  VS_OUT_ASSIGN(l->colors_0);
  VS_OUT_ASSIGN(l->colors_1);
  for (i = 0; i < l->texgens; i++)
    VS_OUT_ASSIGN(l->tex0 + static_cast<int>(i));
  if (!host_config.fast_depth_calc)
    VS_OUT_ASSIGN(l->clipPos);
  if (host_config.per_pixel_lighting)
  {
    VS_OUT_ASSIGN(l->Normal);
    VS_OUT_ASSIGN(l->WorldPos);
  }
  if (host_config.backend_geometry_shaders)
  {
    VS_OUT_ASSIGN(l->clipDist0);
    VS_OUT_ASSIGN(l->clipDist1);
  }
#undef VS_OUT_ASSIGN
}

const struct rslc_type* BuildLightStruct(const struct shader_build* sb)
{
  static const char* const names[5] = {"color", "cosatt", "distatt", "pos", "dir"};
  const struct rslc_type* types[5];
  types[0] = sb->i[4];
  types[1] = sb->f[4];
  types[2] = sb->f[4];
  types[3] = sb->f[4];
  types[4] = sb->f[4];
  return rslc_b_struct(sb->b, "Light", types, names, 5);
}

u32 BuildGeometryShaderUniforms(const struct shader_build* sb, const struct rslc_type** types,
                                const char** names, u32 n)
{
  types[n] = sb->f[4];
  names[n++] = I_STEREOPARAMS;
  types[n] = sb->f[4];
  names[n++] = I_LINEPTPARAMS;
  types[n] = sb->i[4];
  names[n++] = I_TEXOFFSET;
  types[n] = sb->u[1];
  names[n++] = "vs_expand";
  return n;
}

struct rslc_var* BuildLineOffset(const struct shader_build* sb, struct rslc_var* clinept,
                                 sb_expr_fn pos_a, const void* a_ctx, sb_expr_fn pos_b,
                                 const void* b_ctx, struct rslc_var* sign)
{
  // GameCube/Wii's line drawing algorithm is a little quirky. It does not
  // use the correct line caps. Instead, the line caps are vertical or
  // horizontal depending the slope of the line.
  struct rslc_var* offset = rslc_b_local(sb->b, "offset", sb->f[2], 0, false);
  struct rslc_var* to;
  uint32_t then_s, else_s, e;
#define LP(c) sb_swz(sb, sb_ref(sb, clinept), c)
  to = rslc_b_local(
      sb->b, "to", sb->f[2],
      sb_call(sb, "abs", 1,
              sb_bin(sb, RSLC_B_SUB,
                     sb_bin(sb, RSLC_B_DIV, sb_swz(sb, pos_a(sb, a_ctx), "xy"),
                            sb_swz(sb, pos_a(sb, a_ctx), "w")),
                     sb_bin(sb, RSLC_B_DIV, sb_swz(sb, pos_b(sb, b_ctx), "xy"),
                            sb_swz(sb, pos_b(sb, b_ctx), "w")))),
      false);
  // FIXME: What does real hardware do when line is at a 45-degree angle?
  // FIXME: Lines aren't drawn at the correct width. See Twilight Princess map.
  rslc_b_begin_block(sb->b);
  // Line is more tall. Extend geometry left and right.
  // Lerp LineWidth/2 from [0..VpWidth] to [-1..1]
  e = sign ? sb_bin(sb, RSLC_B_MUL, sb_ref(sb, sign), LP("z")) : LP("z");
  e = sb_bin(sb, RSLC_B_DIV, e, LP("x"));
  sb_set(sb, sb_ref(sb, offset), sb_con(sb, sb->f[2], 2, e, sb_i(sb, 0)));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  // Line is more wide. Extend geometry up and down.
  // Lerp LineWidth/2 from [0..VpHeight] to [1..-1]
  e = sb_un(sb, RSLC_B_NEG, LP("z"));
  if (sign)
    e = sb_bin(sb, RSLC_B_MUL, sb_ref(sb, sign), e);
  e = sb_bin(sb, RSLC_B_DIV, e, LP("y"));
  sb_set(sb, sb_ref(sb, offset), sb_con(sb, sb->f[2], 2, sb_i(sb, 0), e));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_GT,
                                      sb_bin(sb, RSLC_B_MUL, LP("y"),
                                             sb_swz(sb, sb_ref(sb, to), "y")),
                                      sb_bin(sb, RSLC_B_MUL, LP("x"),
                                             sb_swz(sb, sb_ref(sb, to), "x"))),
                               then_s, else_s));
#undef LP
  return offset;
}

struct rslc_func* BuildIsNanHeader(const struct shader_build* sb, APIType api_type)
{
  static const char* const names[1] = {"f"};
  static const unsigned dirs[1] = {RSLC_B_IN};
  const struct rslc_type* types[1];
  struct rslc_func* f;
  if (api_type != APIType::D3D)
    return nullptr;
  // Workaround for the HLSL compiler deciding that isnan can never be true and
  // optimising away the call, even though the value can actually be NaN
  // Just look for the bit pattern that indicates NaN instead
  types[0] = sb->f[1];
  f = rslc_b_func(sb->b, "dolphin_isnan", sb->bl, 1, types, dirs, names);
  rslc_b_func_begin(sb->b, f);
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b, sb_bin(sb, RSLC_B_GT,
                                          sb_bin(sb, RSLC_B_BAND,
                                                 sb_call(sb, "floatBitsToInt", 1,
                                                         sb_ref(sb, rslc_b_param(f, 0))),
                                                 sb_i(sb, 0x7FFFFFFF)),
                                          sb_i(sb, 0x7F800000))));
  rslc_b_func_end(sb->b);
  return f;
}

uint32_t sb_isnan(const struct shader_build* sb, const struct rslc_func* isnan_fn, uint32_t e)
{
  if (isnan_fn)
    return rslc_b_call(sb->b, isnan_fn, &e, 1);
  return sb_call(sb, "isnan", 1, e);
}

u32 BuildShaderUniforms(const struct shader_build* sb, const struct rslc_type* light,
                        const struct rslc_type** types, const char** names, u32 n)
{
  static const char* const su_names[SU_COUNT] = {"components",
                                                 "xfmem_dualTexInfo",
                                                 "xfmem_numColorChans",
                                                 "missing_color_hex",
                                                 "missing_color_value",
                                                 I_POSNORMALMATRIX,
                                                 I_PROJECTION,
                                                 I_MATERIALS,
                                                 I_LIGHTS,
                                                 I_TEXMATRICES,
                                                 I_TRANSFORMMATRICES,
                                                 I_NORMALMATRICES,
                                                 I_POSTTRANSFORMMATRICES,
                                                 I_PIXELCENTERCORRECTION,
                                                 I_VIEWPORT_SIZE,
                                                 "xfmem_pack1",
                                                 I_CACHED_NORMAL,
                                                 I_CACHED_TANGENT,
                                                 I_CACHED_BINORMAL,
                                                 "vertex_stride",
                                                 "vertex_offset_rawnormal",
                                                 "vertex_offset_rawtangent",
                                                 "vertex_offset_rawbinormal",
                                                 "vertex_offset_rawpos",
                                                 "vertex_offset_posmtx",
                                                 "vertex_offset_rawcolor0",
                                                 "vertex_offset_rawcolor1",
                                                 "vertex_offset_rawtex"};
  u32 k;
  for (k = 0; k < SU_COUNT; k++)
    names[n + k] = su_names[k];
  types[n + SU_COMPONENTS] = sb->u[1];
  types[n + SU_DUALTEXINFO] = sb->u[1];
  types[n + SU_NUMCOLORCHANS] = sb->u[1];
  types[n + SU_MISSING_COLOR_HEX] = sb->u[1];
  types[n + SU_MISSING_COLOR_VALUE] = sb->f[4];
  types[n + SU_POSNORMALMATRIX] = rslc_b_array(sb->b, sb->f[4], 6);
  types[n + SU_PROJECTION] = rslc_b_array(sb->b, sb->f[4], 4);
  types[n + SU_MATERIALS] = rslc_b_array(sb->b, sb->i[4], 4);
  types[n + SU_LIGHTS] = rslc_b_array(sb->b, light, 8);
  types[n + SU_TEXMATRICES] = rslc_b_array(sb->b, sb->f[4], 24);
  types[n + SU_TRANSFORMMATRICES] = rslc_b_array(sb->b, sb->f[4], 64);
  types[n + SU_NORMALMATRICES] = rslc_b_array(sb->b, sb->f[4], 32);
  types[n + SU_POSTTRANSFORMMATRICES] = rslc_b_array(sb->b, sb->f[4], 64);
  types[n + SU_PIXELCENTERCORRECTION] = sb->f[4];
  types[n + SU_VIEWPORT_SIZE] = sb->f[2];
  types[n + SU_PACK1] = rslc_b_array(sb->b, sb->u[4], 8);
  types[n + SU_CACHED_NORMAL] = sb->f[4];
  types[n + SU_CACHED_TANGENT] = sb->f[4];
  types[n + SU_CACHED_BINORMAL] = sb->f[4];
  for (k = SU_VERTEX_STRIDE; k < SU_OFFSET_RAWTEX; k++)
    types[n + k] = sb->u[1];
  types[n + SU_OFFSET_RAWTEX] = rslc_b_array(sb->b, sb->u[4], 2);
  return n + SU_COUNT;
}

namespace
{
/* o.member */
uint32_t vsx_o(const struct shader_build* sb, const struct vs_expansion* x, int member)
{
  return sb_mem(sb, sb_ref(sb, x->o), static_cast<uint32_t>(member));
}

uint32_t vsx_o_pos(const struct shader_build* sb, const void* ctx)
{
  const struct vs_expansion* x = static_cast<const struct vs_expansion*>(ctx);
  return vsx_o(sb, x, x->layout->pos);
}

uint32_t vsx_other_pos(const struct shader_build* sb, const void* ctx)
{
  return sb_ref(sb, static_cast<const struct vs_expansion*>(ctx)->other_pos);
}

/* o.pos.xy += offset * o.pos.w; */
void vsx_offset_pos(const struct shader_build* sb, const struct vs_expansion* x,
                    struct rslc_var* offset)
{
  sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, vsx_o(sb, x, x->layout->pos), "xy"),
            sb_bin(sb, RSLC_B_MUL, sb_ref(sb, offset),
                   sb_swz(sb, vsx_o(sb, x, x->layout->pos), "w")));
}

/* (((ctexoffset[k] >> i) & 0x1) != 0) */
uint32_t vsx_tex_bit(const struct shader_build* sb, const struct vs_expansion* x, int k, u32 i)
{
  return sb_bin(sb, RSLC_B_NE,
                sb_bin(sb, RSLC_B_BAND,
                       sb_bin(sb, RSLC_B_SHR, sb_idx(sb, sb_ref(sb, x->texoffset), sb_i(sb, k)),
                              sb_i(sb, static_cast<int32_t>(i))),
                       sb_i(sb, 0x1)),
                sb_i(sb, 0));
}

/* is ? a : b, of float literals */
uint32_t vsx_pick(const struct shader_build* sb, struct rslc_var* is, double a, double b)
{
  return rslc_b_select(sb->b, sb_ref(sb, is),
                       a < 0 ? sb_un(sb, RSLC_B_NEG, sb_f(sb, -a)) : sb_f(sb, a),
                       b < 0 ? sb_un(sb, RSLC_B_NEG, sb_f(sb, -b)) : sb_f(sb, b));
}
}  // namespace

void BuildVSLineExpansion(const struct shader_build* sb, const struct vs_expansion* x)
{
  const u32 texgens = x->layout->texgens;
  struct rslc_var *expand_sign, *offset;
  uint32_t d[4];
  int k;
  for (k = 0; k < 4; k++)
    d[k] = sb_call(sb, "dot", 2, sb_idx(sb, sb_ref(sb, x->projection), sb_i(sb, k)),
                   sb_ref(sb, x->other_pos));
  sb_set(sb, sb_ref(sb, x->other_pos), sb_con(sb, sb->f[4], 4, d[0], d[1], d[2], d[3]));
  expand_sign =
      rslc_b_local(sb->b, "expand_sign", sb->f[1], vsx_pick(sb, x->is_right, 1.0, -1.0), false);
  offset = BuildLineOffset(sb, x->clinept, vsx_o_pos, x, vsx_other_pos, x, expand_sign);
  vsx_offset_pos(sb, x, offset);
  if (texgens > 0)
  {
    struct rslc_var* tex_offset;
    uint32_t then_s;
    u32 i;
    rslc_b_begin_block(sb->b);
    tex_offset = rslc_b_local(
        sb->b, "texOffset", sb->f[1],
        sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0),
               sb_con(sb, sb->f[1], 1, sb_idx(sb, sb_ref(sb, x->texoffset), sb_i(sb, 2)))),
        false);
    for (i = 0; i < texgens; i++)
    {
      rslc_b_stmt(sb->b,
                  rslc_b_if(sb->b, vsx_tex_bit(sb, x, 0, i),
                            rslc_b_expr(sb->b, rslc_b_assign(
                                                   sb->b, RSLC_B_ADD,
                                                   sb_swz(sb,
                                                          vsx_o(sb, x,
                                                                x->layout->tex0 +
                                                                    static_cast<int>(i)),
                                                          "x"),
                                                   sb_ref(sb, tex_offset))),
                            0));
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_AND,
                                 sb_bin(sb, RSLC_B_NE,
                                        sb_idx(sb, sb_ref(sb, x->texoffset), sb_i(sb, 2)),
                                        sb_i(sb, 0)),
                                 sb_ref(sb, x->is_right)),
                          then_s, 0));
  }
}

void BuildVSPointExpansion(const struct shader_build* sb, const struct vs_expansion* x)
{
  const u32 texgens = x->layout->texgens;
  struct rslc_var *expand_sign, *offset;
  expand_sign = rslc_b_local(sb->b, "expand_sign", sb->f[2],
                             sb_con(sb, sb->f[2], 2, vsx_pick(sb, x->is_right, 1.0, -1.0),
                                    vsx_pick(sb, x->is_bottom, -1.0, 1.0)),
                             false);
  offset = rslc_b_local(sb->b, "offset", sb->f[2],
                        sb_bin(sb, RSLC_B_DIV,
                               sb_bin(sb, RSLC_B_MUL, sb_ref(sb, expand_sign),
                                      sb_swz(sb, sb_ref(sb, x->clinept), "ww")),
                               sb_swz(sb, sb_ref(sb, x->clinept), "xy")),
                        false);
  vsx_offset_pos(sb, x, offset);
  if (texgens > 0)
  {
    struct rslc_var *magnitude, *tex_offset;
    uint32_t then_s;
    u32 i;
    rslc_b_begin_block(sb->b);
    magnitude = rslc_b_local(
        sb->b, "texOffsetMagnitude", sb->f[1],
        sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0),
               sb_con(sb, sb->f[1], 1, sb_idx(sb, sb_ref(sb, x->texoffset), sb_i(sb, 3)))),
        false);
    tex_offset = rslc_b_local(
        sb->b, "texOffset", sb->f[2],
        sb_con(sb, sb->f[2], 2,
               rslc_b_select(sb->b, sb_ref(sb, x->is_right), sb_ref(sb, magnitude), sb_f(sb, 0.0)),
               rslc_b_select(sb->b, sb_ref(sb, x->is_bottom), sb_ref(sb, magnitude),
                             sb_f(sb, 0.0))),
        false);
    for (i = 0; i < texgens; i++)
    {
      rslc_b_stmt(sb->b,
                  rslc_b_if(sb->b, vsx_tex_bit(sb, x, 1, i),
                            rslc_b_expr(sb->b, rslc_b_assign(
                                                   sb->b, RSLC_B_ADD,
                                                   sb_swz(sb,
                                                          vsx_o(sb, x,
                                                                x->layout->tex0 +
                                                                    static_cast<int>(i)),
                                                          "xy"),
                                                   sb_ref(sb, tex_offset))),
                            0));
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_NE, sb_idx(sb, sb_ref(sb, x->texoffset), sb_i(sb, 3)),
                                 sb_i(sb, 0)),
                          then_s, 0));
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
