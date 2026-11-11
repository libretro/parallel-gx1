// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderGenCommon.h"

#include <stdarg.h>
#include <stdio.h>

#include <cstdio>

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
    char suffix[16];
    snprintf(suffix, sizeof(suffix), "-%06X", static_cast<unsigned>(host_config.bits));
    filename += suffix;

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

void BuildBitfieldExtractHeader(const struct shader_build* sb, const ShaderHostConfig& host_config,
                                struct sb_bitfield* bf)
{
  static const char* const names[3] = {"val", "off", "size"};
  static const unsigned dirs[3] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* types[3];
  struct rslc_var *mask, *val, *off, *size;
  bf->u = bf->i = nullptr;
  // ==============================================
  //  BitfieldExtract for APIs which don't have it
  // ==============================================
  if (host_config.backend_bitfield)
    return;
  // This built-in function is only supported in OpenGL 4.0+ and ES 3.1+
  // Microsoft's HLSL compiler automatically optimises this to a bitfield extract instruction.
  types[0] = sb->u[1];
  types[1] = types[2] = sb->i[1];
  bf->u = rslc_b_func(sb->b, "bitfieldExtract", sb->u[1], 3, types, dirs, names);
  rslc_b_func_begin(sb->b, bf->u);
  val = rslc_b_param(bf->u, 0);
  off = rslc_b_param(bf->u, 1);
  size = rslc_b_param(bf->u, 2);
  mask = rslc_b_local(
      sb->b, "mask", sb->u[1],
      sb_con(sb, sb->u[1], 1,
             sb_bin(sb, RSLC_B_SUB, sb_bin(sb, RSLC_B_SHL, sb_i(sb, 1), sb_ref(sb, size)),
                    sb_i(sb, 1))),
      false);
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b, sb_bin(sb, RSLC_B_BAND,
                                          sb_con(sb, sb->u[1], 1,
                                                 sb_bin(sb, RSLC_B_SHR, sb_ref(sb, val),
                                                        sb_ref(sb, off))),
                                          sb_ref(sb, mask))));
  rslc_b_func_end(sb->b);

  types[0] = sb->i[1];
  bf->i = rslc_b_func(sb->b, "bitfieldExtract", sb->i[1], 3, types, dirs, names);
  rslc_b_func_begin(sb->b, bf->i);
  val = rslc_b_param(bf->i, 0);
  off = rslc_b_param(bf->i, 1);
  size = rslc_b_param(bf->i, 2);
  rslc_b_stmt(
      sb->b,
      rslc_b_return(
          sb->b,
          sb_bin(sb, RSLC_B_SHR,
                 sb_bin(sb, RSLC_B_SHL, sb_ref(sb, val),
                        sb_bin(sb, RSLC_B_SUB,
                               sb_bin(sb, RSLC_B_SUB, sb_i(sb, 32), sb_ref(sb, size)),
                               sb_ref(sb, off))),
                 sb_bin(sb, RSLC_B_SUB, sb_i(sb, 32), sb_ref(sb, size)))));
  rslc_b_func_end(sb->b);
}

uint32_t sb_bitfield_extract(const struct shader_build* sb, const struct sb_bitfield* bf,
                             bool is_signed, uint32_t src, u32 start, u32 bits)
{
  uint32_t a[3];
  const struct rslc_func* f = is_signed ? bf->i : bf->u;
  a[0] = sb_con(sb, is_signed ? sb->i[1] : sb->u[1], 1, src);
  a[1] = sb_i(sb, static_cast<int32_t>(start));
  a[2] = sb_i(sb, static_cast<int32_t>(bits));
  if (f)
    return rslc_b_call(sb->b, f, a, 3);
  return rslc_b_builtin(sb->b, "bitfieldExtract", a, 3);
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
