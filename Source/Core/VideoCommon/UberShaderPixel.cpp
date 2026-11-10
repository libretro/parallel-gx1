// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/UberShaderPixel.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>


#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderBuild.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/UberShaderCommon.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
PixelShaderUid GetPixelShaderUid()
{
  PixelShaderUid out;

  pixel_ubershader_uid_data* const uid_data = out.GetUidData();
  uid_data->num_texgens = xfmem.numTexGen.numTexGens;
  uid_data->early_depth = bpmem.GetEmulatedZ() == EmulatedZ::Early &&
                          (g_ActiveConfig.bFastDepthCalc ||
                           bpmem.alpha_test.TestResult() == AlphaTestResult::Undetermined) &&
                          !(bpmem.zmode.test_enable && bpmem.genMode.zfreeze);
  uid_data->per_pixel_depth =
      (bpmem.ztex2.op != ZTexOp::Disabled && bpmem.GetEmulatedZ() == EmulatedZ::Late) ||
      (!g_ActiveConfig.bFastDepthCalc && bpmem.zmode.test_enable && !uid_data->early_depth) ||
      (bpmem.zmode.test_enable && bpmem.genMode.zfreeze);
  uid_data->uint_output = bpmem.blendmode.UseLogicOp();

  return out;
}

void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid)
{
  pixel_ubershader_uid_data* const uid_data = uid->GetUidData();

  // With fbfetch, ubershaders always blend using that and don't use dual src
  if (host_config.backend_shader_framebuffer_fetch || !host_config.backend_dual_source_blend)
    uid_data->no_dual_src = 1;
  // Dual source is always enabled in the shader if this bug is not present
  else if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING))
    uid_data->no_dual_src = 0;

  // OpenGL and Vulkan convert implicitly normalized color outputs to their uint representation.
  // Therefore, it is not necessary to use a uint output on these backends. We also disable the
  // uint output when logic op is not supported (i.e. driver/device does not support D3D11.1).
  if (api_type != APIType::D3D || !host_config.backend_logic_op)
    uid_data->uint_output = 0;
}

u32 EnumeratePixelShaderUids(PixelShaderUid* uids)
{
  u32 count = 0;
  u32 texgens, early_depth, per_pixel_depth, uint_output, no_dual_src;

  for (texgens = 0; texgens <= 8; texgens++)
  {
    for (early_depth = 0; early_depth < 2; early_depth++)
    {
      for (per_pixel_depth = 0; per_pixel_depth < 2; per_pixel_depth++)
      {
        // Don't generate shaders where we have early depth tests enabled, and write gl_FragDepth.
        if (early_depth && per_pixel_depth)
          continue;
        for (uint_output = 0; uint_output < 2; uint_output++)
        {
          for (no_dual_src = 0; no_dual_src < 2; no_dual_src++)
          {
            pixel_ubershader_uid_data* const puid = uids[count].GetUidData();
            uids[count] = PixelShaderUid();
            puid->num_texgens = texgens;
            puid->early_depth = early_depth != 0;
            puid->per_pixel_depth = per_pixel_depth != 0;
            puid->uint_output = uint_output;
            puid->no_dual_src = no_dual_src;
            count++;
          }
        }
      }
    }
  }
  return count;
}
}  // namespace UberShader

/* ---- built by calls ---- */

namespace UberShader
{
namespace
{
/* State's and StageState's members */
enum
{
  ST_REG,
  ST_RAW_TEX_COLOR,
  ST_TEX_COLOR,
  ST_ALPHA_BUMP
};
enum
{
  SS_STAGE,
  SS_ORDER,
  SS_CC,
  SS_AC
};

struct up_build
{
  const struct shader_build* sb;
  const ShaderHostConfig* host_config;
  const pixel_ubershader_uid_data* uid_data;
  APIType api_type;
  u32 numtexgen;
  struct sb_bitfield bitfield;
  struct ps_common c;
  struct ps_inputs in;
  struct vs_output_layout plain;
  const struct rslc_type* state_t;
  const struct rslc_type* stage_state_t;
  struct rslc_func* select_tex_coord;
  struct rslc_func* sample_wrapper;
  struct rslc_func* swizzle;
  struct rslc_func* wrap;
  struct rslc_func* tev_lerp[2];
  struct rslc_func* tev_compare;
  struct rslc_func* alpha_compare;
  struct rslc_func* select_color_input;
  struct rslc_func* select_alpha_input;
  struct rslc_func* get_tev_reg;
  struct rslc_func* get_ras_color;
  struct rslc_func* get_konst_color;
};

#define UP_BFE(field, src) BuildBitfieldExtract<&field>(sb, &u->bitfield, src)
#define UP_PSU(k) sb_ref(sb, u->c.ps[k])

uint32_t up_r(const struct shader_build* sb, const struct rslc_var* v)
{
  return sb_ref(sb, v);
}

/* a literal int as the text writes it */
uint32_t up_int(const struct shader_build* sb, int v)
{
  return v < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -v)) : sb_i(sb, v);
}

uint32_t up_ivec(const struct shader_build* sb, unsigned n, int v)
{
  uint32_t a[4];
  unsigned k;
  for (k = 0; k < n; k++)
    a[k] = up_int(sb, v);
  return rslc_b_construct(sb->b, sb->i[n], a, n);
}

/* l = r; as a statement not yet appended */
uint32_t up_set_stmt(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  return rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r));
}

void up_return(const struct shader_build* sb, uint32_t e)
{
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, e));
}

/* { return e; } */
uint32_t up_return_block(const struct shader_build* sb, uint32_t e)
{
  rslc_b_begin_block(sb->b);
  up_return(sb, e);
  return rslc_b_end_block(sb->b);
}

/* { l = r; } */
uint32_t up_set_block(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  rslc_b_begin_block(sb->b);
  sb_set(sb, l, r);
  return rslc_b_end_block(sb->b);
}

uint32_t up_call(const struct shader_build* sb, const struct rslc_func* f, unsigned n, ...)
{
  uint32_t a[12];
  unsigned k;
  va_list ap;
  va_start(ap, n);
  for (k = 0; k < n && k < 12; k++)
    a[k] = va_arg(ap, uint32_t);
  va_end(ap);
  return rslc_b_call(sb->b, f, a, n);
}

/* bitfieldExtract(src, start, bits) as the text calls it bare: the header's own where it
 * declares one */
uint32_t up_bfe_raw(const struct up_build* u, uint32_t src, int start, int bits)
{
  const struct shader_build* sb = u->sb;
  uint32_t a[3];
  a[0] = src;
  a[1] = sb_i(sb, start);
  a[2] = sb_i(sb, bits);
  if (u->bitfield.u)
    return rslc_b_call(sb->b, u->bitfield.u, a, 3);
  return rslc_b_builtin(sb->b, "bitfieldExtract", a, 3);
}

/* bpmem_pack1[i].c, bpmem_pack2[i].c: the bpmem_* accessors */
uint32_t up_pack1(const struct up_build* u, uint32_t i, const char* c)
{
  const struct shader_build* sb = u->sb;
  return sb_swz(sb, sb_idx(sb, UP_PSU(PSU_PACK1), i), c);
}

uint32_t up_pack2(const struct up_build* u, uint32_t i, const char* c)
{
  const struct shader_build* sb = u->sb;
  return sb_swz(sb, sb_idx(sb, UP_PSU(PSU_PACK2), i), c);
}

/* A new function of n parameters, all in, its body begun */
struct rslc_func* up_func(const struct shader_build* sb, const char* name,
                          const struct rslc_type* ret, unsigned n,
                          const struct rslc_type* const* types, const char* const* names,
                          bool begin)
{
  static const unsigned dirs[12] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN,
                                    RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN,
                                    RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  struct rslc_func* f = rslc_b_func(sb->b, name, ret, n, types, dirs, names);
  if (f && begin)
    rslc_b_func_begin(sb->b, f);
  return f;
}

/* A tree of ifs on var, leaf(i) where it is i, low <= i < high */
typedef void (*up_leaf_fn)(const struct up_build* u, u32 i, const void* ctx);
void up_switch_tree(const struct up_build* u, const struct rslc_var* var, u32 low, u32 high,
                    up_leaf_fn leaf, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  uint32_t then_s, else_s;
  u32 mid;
  if (high == low + 1)
  {
    leaf(u, low, ctx);
    return;
  }
  mid = low + ((high - low) / 2);
  rslc_b_begin_block(sb->b);
  up_switch_tree(u, var, low, mid, leaf, ctx);
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  up_switch_tree(u, var, mid, high, leaf, ctx);
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, var), sb_u(sb, mid)), then_s, else_s));
}

/* return (index == a) ? x : y; or return x; for the texgen selection trees */
typedef uint32_t (*up_pick_fn)(const struct up_build* u, u32 i, const void* ctx);
void up_return_pick(const struct up_build* u, const struct rslc_var* index, u32 a, bool pair,
                    up_pick_fn pick, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  if (pair)
    up_return(sb, rslc_b_select(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, index), sb_u(sb, a)),
                                pick(u, a, ctx), pick(u, a + 1, ctx)));
  else
    up_return(sb, pick(u, a, ctx));
}

/* selectTexCoord's and selectTexCoordIndex's bodies: on D3D a switch, elsewhere a tree */
void up_select_body(const struct up_build* u, const struct rslc_var* index, up_pick_fn pick,
                    const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const u32 n = u->numtexgen;
  uint32_t then_s, else_s, inner_then, inner_else;
  u32 i;

  /* the draw's texgens, which the shader may be sized for more of */
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_GE, up_r(sb, index),
                                      UP_PSU(PSU_NUMTEXGEN)),
                               up_return_block(sb, pick(u, 0, ctx)), 0));
  if (u->api_type == APIType::D3D)
  {
    uint32_t body;
    rslc_b_begin_block(sb->b);
    for (i = 0; i < n; i++)
    {
      rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(i)));
      up_return(sb, pick(u, i, ctx));
    }
    rslc_b_stmt(sb->b, rslc_b_default(sb->b));
    up_return(sb, pick(u, 0, ctx));
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, index), body));
    return;
  }

  /* index < 4u: index < 2u, else; or the pair alone */
  if (n > 4)
    rslc_b_begin_block(sb->b);
  if (n > 2)
  {
    rslc_b_begin_block(sb->b);
    up_return_pick(u, index, 0, n > 1, pick, ctx);
    inner_then = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    up_return_pick(u, index, 2, n > 3, pick, ctx);
    inner_else = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, index), sb_u(sb, 2)),
                                 inner_then, inner_else));
  }
  else
  {
    up_return_pick(u, index, 0, n > 1, pick, ctx);
  }
  if (n > 4)
  {
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    if (n > 6)
    {
      rslc_b_begin_block(sb->b);
      up_return_pick(u, index, 4, n > 5, pick, ctx);
      inner_then = rslc_b_end_block(sb->b);
      rslc_b_begin_block(sb->b);
      up_return_pick(u, index, 6, n > 7, pick, ctx);
      inner_else = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, index), sb_u(sb, 6)),
                                   inner_then, inner_else));
    }
    else
    {
      up_return_pick(u, index, 4, n > 5, pick, ctx);
    }
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, index), sb_u(sb, 4)),
                                 then_s, else_s));
  }
}

uint32_t up_pick_uv(const struct up_build* u, u32 i, const void* ctx)
{
  const struct rslc_func* f = static_cast<const struct rslc_func*>(ctx);
  return sb_ref(u->sb, rslc_b_param(f, i + 1));
}

uint32_t up_pick_index(const struct up_build* u, u32 i, const void* /* ctx */)
{
  return sb_u(u->sb, i);
}

/* selectTexCoord, selectTexCoordIndex */
void up_select_functions(struct up_build* u)
{
  static const char* const uv_names[9] = {"index",        "fixpoint_uv0", "fixpoint_uv1",
                                          "fixpoint_uv2", "fixpoint_uv3", "fixpoint_uv4",
                                          "fixpoint_uv5", "fixpoint_uv6", "fixpoint_uv7"};
  static const char* const index_names[1] = {"texmap"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[9];
  struct rslc_func* f;
  u32 i;

  types[0] = sb->u[1];
  for (i = 0; i < u->numtexgen; i++)
    types[i + 1] = sb->i[2];
  f = up_func(sb, "selectTexCoord", sb->i[2], u->numtexgen + 1, types, uv_names, true);
  u->select_tex_coord = f;
  up_select_body(u, rslc_b_param(f, 0), up_pick_uv, f);
  rslc_b_func_end(sb->b);

  f = up_func(sb, "selectTexCoordIndex", sb->u[1], 1, types, index_names, true);
  up_select_body(u, rslc_b_param(f, 0), up_pick_index, nullptr);
  rslc_b_func_end(sb->b);
}

/* sampleTextureWrapper */
void up_sample_wrapper(struct up_build* u)
{
  static const char* const dyn_names[3] = {"texmap", "uv", "layer"};
  static const char* const names[3] = {"sampler_num", "uv", "layer"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[3];
  struct rslc_func* f;

  types[0] = sb->u[1];
  types[1] = sb->i[2];
  types[2] = sb->i[1];
  if (u->host_config->backend_dynamic_sampler_indexing)
  {
    // Doesn't look like DirectX supports this. Oh well the code path is here just in case it
    // supports this in the future.
    f = up_func(sb, "sampleTextureWrapper", sb->i[4], 3, types, dyn_names, true);
    up_return(sb, up_call(sb, u->c.sample_texture, 4, up_r(sb, rslc_b_param(f, 0)),
                          sb_idx(sb, up_r(sb, u->c.samp), up_r(sb, rslc_b_param(f, 0))),
                          up_r(sb, rslc_b_param(f, 1)), up_r(sb, rslc_b_param(f, 2))));
  }
  else
  {
    // This is messy, but DirectX, OpenGL 3.3, and OpenGL ES 3.0 don't support dynamic indexing
    // of the sampler array. With any luck the shader compiler will optimise this if the hardware
    // supports dynamic indexing.
    uint32_t body;
    u32 i;
    f = up_func(sb, "sampleTextureWrapper", sb->i[4], 3, types, names, true);
    rslc_b_begin_block(sb->b);
    for (i = 0; i < 8; i++)
    {
      rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(i)));
      up_return(sb, up_call(sb, u->c.sample_texture, 4, sb_u(sb, i),
                            sb_idx(sb, up_r(sb, u->c.samp), sb_u(sb, i)),
                            up_r(sb, rslc_b_param(f, 1)), up_r(sb, rslc_b_param(f, 2))));
    }
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, rslc_b_param(f, 0)), body));
  }
  rslc_b_func_end(sb->b);
  u->sample_wrapper = f;
}

/* Swizzle: AKA color channel swapping */
void up_swizzle(struct up_build* u)
{
  static const char* const names[2] = {"s", "color"};
  static const char* const comps[4] = {"r", "g", "b", "a"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[2];
  struct rslc_func* f;
  struct rslc_var *s, *color, *ret;
  u32 k;

  types[0] = sb->u[1];
  types[1] = sb->i[4];
  f = up_func(sb, "Swizzle", sb->i[4], 2, types, names, true);
  s = rslc_b_param(f, 0);
  color = rslc_b_param(f, 1);
  ret = rslc_b_local(sb->b, "ret", sb->i[4], 0, false);
  for (k = 0; k < 4; k++)
  {
    uint32_t idx = sb_bin(sb, RSLC_B_MUL, up_r(sb, s), sb_u(sb, 2));
    uint32_t ksel;
    if (k >= 2)
      idx = sb_bin(sb, RSLC_B_ADD, idx, sb_u(sb, 1));
    ksel = up_pack2(u, idx, "y");
    sb_set(sb, sb_swz(sb, up_r(sb, ret), comps[k]),
           sb_idx(sb, up_r(sb, color),
                  (k & 1) ? UP_BFE(TevKSel::swap_ga, ksel) : UP_BFE(TevKSel::swap_rb, ksel)));
  }
  up_return(sb, up_r(sb, ret));
  rslc_b_func_end(sb->b);
  u->swizzle = f;
}

/* Wrap: indirect wrapping */
void up_wrap(struct up_build* u)
{
  static const char* const names[2] = {"coord", "mode"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[2];
  struct rslc_func* f;
  struct rslc_var *coord, *mode;
  uint32_t inner;

  types[0] = sb->i[1];
  types[1] = sb->u[1];
  f = up_func(sb, "Wrap", sb->i[1], 2, types, names, true);
  coord = rslc_b_param(f, 0);
  mode = rslc_b_param(f, 1);
  inner = rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, mode), sb_u(sb, 6)),  // ITW_256 to ITW_16
                    rslc_b_return(sb->b, sb_bin(sb, RSLC_B_BAND, up_r(sb, coord),
                                                sb_bin(sb, RSLC_B_SHR, sb_i(sb, 0xfffe),
                                                       up_r(sb, mode)))),
                    rslc_b_return(sb->b, sb_i(sb, 0)));  // ITW_0
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, mode), sb_u(sb, 0)),  // ITW_OFF
                               rslc_b_return(sb->b, up_r(sb, coord)), inner));
  rslc_b_func_end(sb->b);
  u->wrap = f;
}

/* tevLerp, tevLerp3: TEV's linear interpolate, plus bias, add/subtract and scale */
struct rslc_func* up_tev_lerp(const struct up_build* u, unsigned n)
{
  static const char* const names[7] = {"A", "B", "C", "D", "bias", "op", "scale"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[7];
  struct rslc_func* f;
  struct rslc_var *A, *B, *C, *D, *bias, *op, *scale, *lerp, *result;

  types[0] = types[1] = types[2] = types[3] = sb->i[n];
  types[4] = sb->u[1];
  types[5] = sb->bl;
  types[6] = sb->u[1];
  f = up_func(sb, n == 1 ? "tevLerp" : "tevLerp3", sb->i[n], 7, types, names, true);
  A = rslc_b_param(f, 0);
  B = rslc_b_param(f, 1);
  C = rslc_b_param(f, 2);
  D = rslc_b_param(f, 3);
  bias = rslc_b_param(f, 4);
  op = rslc_b_param(f, 5);
  scale = rslc_b_param(f, 6);
  // Scale C from 0..255 to 0..256
  sb_op_set(sb, RSLC_B_ADD, up_r(sb, C), sb_bin(sb, RSLC_B_SHR, up_r(sb, C), sb_i(sb, 7)));
  // Add bias to D
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, bias), sb_u(sb, 1)),
                        rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_ADD, up_r(sb, D),
                                                         sb_i(sb, 128))),
                        rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, bias), sb_u(sb, 2)),
                                  rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SUB,
                                                                   up_r(sb, D), sb_i(sb, 128))),
                                  0)));
  /* "lerp" in the text, which the headers' #define lerp mix made "mix" */
  lerp = rslc_b_local(
      sb->b, "mix", sb->i[n],
      sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_SHL, up_r(sb, A), sb_i(sb, 8)),
             sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_SUB, up_r(sb, B), up_r(sb, A)),
                    up_r(sb, C))),
      false);
  {
    uint32_t then_s;
    rslc_b_begin_block(sb->b);
    sb_set(sb, up_r(sb, lerp), sb_bin(sb, RSLC_B_SHL, up_r(sb, lerp), up_r(sb, scale)));
    sb_set(sb, up_r(sb, D), sb_bin(sb, RSLC_B_SHL, up_r(sb, D), up_r(sb, scale)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, up_r(sb, scale), sb_u(sb, 3)), then_s, 0));
  }
  // This rounding bias is not added when the scale is divide by 2
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, up_r(sb, scale), sb_u(sb, 3)),
                        up_set_stmt(sb, up_r(sb, lerp),
                                    sb_bin(sb, RSLC_B_ADD, up_r(sb, lerp),
                                           rslc_b_select(sb->b, up_r(sb, op), sb_i(sb, 127),
                                                         sb_i(sb, 128)))),
                        0));
  result = rslc_b_local(sb->b, "result", sb->i[n],
                        sb_bin(sb, RSLC_B_SHR, up_r(sb, lerp), sb_i(sb, 8)), false);
  // Add/Subtract D
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, up_r(sb, op),
                        up_set_stmt(sb, up_r(sb, result),
                                    sb_bin(sb, RSLC_B_SUB, up_r(sb, D), up_r(sb, result))),
                        up_set_stmt(sb, up_r(sb, result),
                                    sb_bin(sb, RSLC_B_ADD, up_r(sb, D), up_r(sb, result)))));
  // Most of the Scale was moved inside the lerp for improved precision
  // But we still do the divide by 2 here
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, scale), sb_u(sb, 3)),
                        up_set_stmt(sb, up_r(sb, result),
                                    sb_bin(sb, RSLC_B_SHR, up_r(sb, result), sb_i(sb, 1))),
                        0));
  up_return(sb, up_r(sb, result));
  rslc_b_func_end(sb->b);
  return f;
}

/* tevCompare: operations 0-5 of TEV's compare mode, common to both color and alpha channels */
void up_tev_compare(struct up_build* u)
{
  static const char* const names[3] = {"op", "color_A", "color_B"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[3];
  struct rslc_func* f;
  struct rslc_var *op, *ca, *cb, *a16, *b16, *a24, *b24;
  uint32_t body;

#define CA(c) sb_swz(sb, up_r(sb, ca), c)
#define CB(c) sb_swz(sb, up_r(sb, cb), c)
  types[0] = sb->u[1];
  types[1] = types[2] = sb->i[3];
  f = up_func(sb, "tevCompare", sb->bl, 3, types, names, true);
  op = rslc_b_param(f, 0);
  ca = rslc_b_param(f, 1);
  cb = rslc_b_param(f, 2);
  rslc_b_begin_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 0));  // TevCompareMode::R8, TevComparison::GT
  up_return(sb, sb_bin(sb, RSLC_B_GT, CA("r"), CB("r")));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 1));  // TevCompareMode::R8, TevComparison::EQ
  up_return(sb, sb_bin(sb, RSLC_B_EQ, CA("r"), CB("r")));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 2));  // TevCompareMode::GR16, TevComparison::GT
  a16 = rslc_b_local(sb->b, "A_16", sb->i[1],
                     sb_bin(sb, RSLC_B_BOR, CA("r"), sb_bin(sb, RSLC_B_SHL, CA("g"), sb_i(sb, 8))),
                     false);
  b16 = rslc_b_local(sb->b, "B_16", sb->i[1],
                     sb_bin(sb, RSLC_B_BOR, CB("r"), sb_bin(sb, RSLC_B_SHL, CB("g"), sb_i(sb, 8))),
                     false);
  up_return(sb, sb_bin(sb, RSLC_B_GT, up_r(sb, a16), up_r(sb, b16)));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 3));  // TevCompareMode::GR16, TevComparison::EQ
  up_return(sb, sb_bin(sb, RSLC_B_AND, sb_bin(sb, RSLC_B_EQ, CA("r"), CB("r")),
                       sb_bin(sb, RSLC_B_EQ, CA("g"), CB("g"))));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 4));  // TevCompareMode::BGR24, TevComparison::GT
  a24 = rslc_b_local(sb->b, "A_24", sb->i[1],
                     sb_bin(sb, RSLC_B_BOR,
                            sb_bin(sb, RSLC_B_BOR, CA("r"),
                                   sb_bin(sb, RSLC_B_SHL, CA("g"), sb_i(sb, 8))),
                            sb_bin(sb, RSLC_B_SHL, CA("b"), sb_i(sb, 16))),
                     false);
  b24 = rslc_b_local(sb->b, "B_24", sb->i[1],
                     sb_bin(sb, RSLC_B_BOR,
                            sb_bin(sb, RSLC_B_BOR, CB("r"),
                                   sb_bin(sb, RSLC_B_SHL, CB("g"), sb_i(sb, 8))),
                            sb_bin(sb, RSLC_B_SHL, CB("b"), sb_i(sb, 16))),
                     false);
  up_return(sb, sb_bin(sb, RSLC_B_GT, up_r(sb, a24), up_r(sb, b24)));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, 5));  // TevCompareMode::BGR24, TevComparison::EQ
  up_return(sb, sb_bin(sb, RSLC_B_AND,
                       sb_bin(sb, RSLC_B_AND, sb_bin(sb, RSLC_B_EQ, CA("r"), CB("r")),
                              sb_bin(sb, RSLC_B_EQ, CA("g"), CB("g"))),
                       sb_bin(sb, RSLC_B_EQ, CA("b"), CB("b"))));
  rslc_b_stmt(sb->b, rslc_b_default(sb->b));
  up_return(sb, rslc_b_bool(sb->b, false));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, op), body));
  rslc_b_func_end(sb->b);
  u->tev_compare = f;
#undef CA
#undef CB
}

/* the leaves of alphaCompare, selectColorInput, selectAlphaInput and getTevReg */
struct up_select_ctx
{
  const struct rslc_func* f;
};

void up_alpha_func_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  static const enum rslc_b_binop ops[8] = {RSLC_B_SET, RSLC_B_LT, RSLC_B_EQ, RSLC_B_LE,
                                           RSLC_B_GT,  RSLC_B_NE, RSLC_B_GE, RSLC_B_SET};
  const struct shader_build* sb = u->sb;
  const struct rslc_func* f = static_cast<const struct up_select_ctx*>(ctx)->f;
  if (i == 0 || i == 7)  // Never, Always
    up_return(sb, rslc_b_bool(sb->b, i == 7));
  else
    up_return(sb, sb_bin(sb, ops[i], up_r(sb, rslc_b_param(f, 0)), up_r(sb, rslc_b_param(f, 1))));
}

/* s.Reg[r] */
uint32_t up_reg(const struct shader_build* sb, const struct rslc_var* s, int r)
{
  return sb_idx(sb, sb_mem(sb, up_r(sb, s), ST_REG), sb_i(sb, r));
}

/* getRasColor(s, ss, colors_0, colors_1), getKonstColor(s, ss): of the parameters of f */
uint32_t up_ras(const struct up_build* u, const struct rslc_func* f)
{
  const struct shader_build* sb = u->sb;
  return up_call(sb, u->get_ras_color, 4, up_r(sb, rslc_b_param(f, 0)),
                 up_r(sb, rslc_b_param(f, 1)), up_r(sb, rslc_b_param(f, 2)),
                 up_r(sb, rslc_b_param(f, 3)));
}

uint32_t up_konst(const struct up_build* u, const struct rslc_func* f)
{
  const struct shader_build* sb = u->sb;
  return up_call(sb, u->get_konst_color, 2, up_r(sb, rslc_b_param(f, 0)),
                 up_r(sb, rslc_b_param(f, 1)));
}

uint32_t up_ivec3(const struct shader_build* sb, int v)
{
  return sb_con(sb, sb->i[3], 3, sb_i(sb, v), sb_i(sb, v), sb_i(sb, v));
}

void up_color_input_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const struct rslc_func* f = static_cast<const struct up_select_ctx*>(ctx)->f;
  const struct rslc_var* s = rslc_b_param(f, 0);
  const char* c = (i & 1) ? "aaa" : "rgb";
  uint32_t e;
  if (i < 8)  // CPREV, APREV, C0, A0, C1, A1, C2, A2
    e = sb_swz(sb, up_reg(sb, s, static_cast<int>(i / 2)), c);
  else if (i < 10)  // TEXC, TEXA
    e = sb_swz(sb, sb_mem(sb, up_r(sb, s), ST_TEX_COLOR), c);
  else if (i < 12)  // RASC, RASA
    e = sb_swz(sb, up_ras(u, f), c);
  else if (i == 12)  // ONE
    e = up_ivec3(sb, 255);
  else if (i == 13)  // HALF
    e = up_ivec3(sb, 128);
  else if (i == 14)  // KONST
    e = sb_swz(sb, up_konst(u, f), "rgb");
  else  // ZERO
    e = up_ivec3(sb, 0);
  up_return(sb, e);
}

void up_alpha_input_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const struct rslc_func* f = static_cast<const struct up_select_ctx*>(ctx)->f;
  const struct rslc_var* s = rslc_b_param(f, 0);
  uint32_t e;
  if (i < 4)  // APREV, A0, A1, A2
    e = sb_swz(sb, up_reg(sb, s, static_cast<int>(i)), "a");
  else if (i == 4)  // TEXA
    e = sb_swz(sb, sb_mem(sb, up_r(sb, s), ST_TEX_COLOR), "a");
  else if (i == 5)  // RASA
    e = sb_swz(sb, up_ras(u, f), "a");
  else if (i == 6)  // KONST (hw1 had quarter)
    e = sb_swz(sb, up_konst(u, f), "a");
  else  // ZERO
    e = sb_i(sb, 0);
  up_return(sb, e);
}

void up_tev_reg_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct rslc_func* f = static_cast<const struct up_select_ctx*>(ctx)->f;
  up_return(u->sb, up_reg(u->sb, rslc_b_param(f, 0), static_cast<int>(i)));
}

/* State, StageState, getRasColor's and getKonstColor's declarations, alphaCompare,
 * selectColorInput, selectAlphaInput, getTevReg */
void up_input_selects(struct up_build* u)
{
  static const char* const state_names[4] = {"Reg", "RawTexColor", "TexColor", "AlphaBump"};
  static const char* const stage_names[4] = {"stage", "order", "cc", "ac"};
  static const char* const ras_names[4] = {"s", "ss", "colors_0", "colors_1"};
  static const char* const ac_names[3] = {"a", "b", "compare"};
  static const char* const sel_names[5] = {"s", "ss", "colors_0", "colors_1", "index"};
  static const char* const reg_names[2] = {"s", "index"};
  const struct shader_build* sb = u->sb;
  const struct rslc_type* types[5];
  struct up_select_ctx ctx;
  struct rslc_func* f;

  types[0] = rslc_b_array(sb->b, sb->i[4], 4);
  types[1] = types[2] = sb->i[4];
  types[3] = sb->i[1];
  u->state_t = rslc_b_struct(sb->b, "State", types, state_names, 4);
  types[0] = types[1] = types[2] = types[3] = sb->u[1];
  u->stage_state_t = rslc_b_struct(sb->b, "StageState", types, stage_names, 4);

  types[0] = u->state_t;
  types[1] = u->stage_state_t;
  types[2] = types[3] = sb->f[4];
  u->get_ras_color = up_func(sb, "getRasColor", sb->i[4], 4, types, ras_names, false);
  u->get_konst_color = up_func(sb, "getKonstColor", sb->i[4], 2, types, ras_names, false);

  // Helper function for Alpha Test
  types[0] = types[1] = sb->i[1];
  types[2] = sb->u[1];
  f = up_func(sb, "alphaCompare", sb->bl, 3, types, ac_names, true);
  ctx.f = f;
  up_switch_tree(u, rslc_b_param(f, 2), 0, 8, up_alpha_func_leaf, &ctx);
  rslc_b_func_end(sb->b);
  u->alpha_compare = f;

  types[0] = u->state_t;
  types[1] = u->stage_state_t;
  types[2] = types[3] = sb->f[4];
  types[4] = sb->u[1];
  f = up_func(sb, "selectColorInput", sb->i[3], 5, types, sel_names, true);
  ctx.f = f;
  up_switch_tree(u, rslc_b_param(f, 4), 0, 16, up_color_input_leaf, &ctx);
  rslc_b_func_end(sb->b);
  u->select_color_input = f;

  f = up_func(sb, "selectAlphaInput", sb->i[1], 5, types, sel_names, true);
  ctx.f = f;
  up_switch_tree(u, rslc_b_param(f, 4), 0, 8, up_alpha_input_leaf, &ctx);
  rslc_b_func_end(sb->b);
  u->select_alpha_input = f;

  types[1] = sb->u[1];
  f = up_func(sb, "getTevReg", sb->i[4], 2, types, reg_names, true);
  ctx.f = f;
  up_switch_tree(u, rslc_b_param(f, 1), 0, 4, up_tev_reg_leaf, &ctx);
  rslc_b_func_end(sb->b);
  u->get_tev_reg = f;
}

/* getRasColor's and getKonstColor's bodies, after main as the text has them */
void up_ras_konst_bodies(const struct up_build* u)
{
  const struct shader_build* sb = u->sb;
  struct rslc_func* f = u->get_ras_color;
  struct rslc_var *s, *ss, *ras, *color, *swap, *normalized, *tevksel;
  uint32_t then_s, else_s, e;

  rslc_b_func_begin(sb->b, f);
  s = rslc_b_param(f, 0);
  ss = rslc_b_param(f, 1);
  // Select Ras for stage
  ras = rslc_b_local(sb->b, "ras", sb->u[1],
                     UP_BFE(TwoTevStageOrders::colorchan_even, sb_mem(sb, up_r(sb, ss), SS_ORDER)),
                     false);
  // Lighting Channel 0 or 1
  rslc_b_begin_block(sb->b);
  e = rslc_b_select(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, ras), sb_u(sb, 0)),
                    up_r(sb, rslc_b_param(f, 2)), up_r(sb, rslc_b_param(f, 3)));
  e = sb_bin(sb, RSLC_B_MUL, e, sb_f(sb, 255.0));
  color = rslc_b_local(sb->b, "color", sb->i[4], rslc_b_call(sb->b, u->c.iround[4], &e, 1), false);
  swap = rslc_b_local(sb->b, "swap", sb->u[1],
                      UP_BFE(TevStageCombiner::AlphaCombiner::rswap,
                             sb_mem(sb, up_r(sb, ss), SS_AC)),
                      false);
  up_return(sb, up_call(sb, u->swizzle, 2, up_r(sb, swap), up_r(sb, color)));
  then_s = rslc_b_end_block(sb->b);
  {
    uint32_t bump_then, norm_then, last;
    uint32_t bump = sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP);
    // Alpha Bump
    bump_then = up_return_block(
        sb, sb_con(sb, sb->i[4], 4, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP),
                   sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP), sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP),
                   sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP)));
    // Normalized Alpha Bump
    rslc_b_begin_block(sb->b);
    normalized = rslc_b_local(
        sb->b, "normalized", sb->i[1],
        sb_bin(sb, RSLC_B_BOR, bump,
               sb_bin(sb, RSLC_B_SHR, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP), sb_i(sb, 5))),
        false);
    up_return(sb, sb_con(sb, sb->i[4], 4, up_r(sb, normalized), up_r(sb, normalized),
                         up_r(sb, normalized), up_r(sb, normalized)));
    norm_then = rslc_b_end_block(sb->b);
    last = up_return_block(sb, up_ivec(sb, 4, 0));
    else_s = rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, ras), sb_u(sb, 5)), bump_then,
                       rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, ras), sb_u(sb, 6)),
                                 norm_then, last));
  }
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, up_r(sb, ras), sb_u(sb, 2)), then_s, else_s));
  rslc_b_func_end(sb->b);

  // Select Konst for stage
  f = u->get_konst_color;
  rslc_b_func_begin(sb->b, f);
  ss = rslc_b_param(f, 1);
  tevksel = rslc_b_local(
      sb->b, "tevksel", sb->u[1],
      up_pack2(u, sb_bin(sb, RSLC_B_SHR, sb_mem(sb, up_r(sb, ss), SS_STAGE), sb_i(sb, 1)), "y"),
      false);
#define KONST(field) sb_idx(sb, UP_PSU(PSU_KONSTLOOKUP), UP_BFE(field, up_r(sb, tevksel)))
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b,
                sb_bin(sb, RSLC_B_EQ,
                       sb_bin(sb, RSLC_B_BAND, sb_mem(sb, up_r(sb, ss), SS_STAGE), sb_u(sb, 1)),
                       sb_u(sb, 0)),
                rslc_b_return(sb->b,
                              sb_con(sb, sb->i[4], 2, sb_swz(sb, KONST(TevKSel::kcsel_even), "rgb"),
                                     sb_swz(sb, KONST(TevKSel::kasel_even), "a"))),
                rslc_b_return(sb->b,
                              sb_con(sb, sb->i[4], 2, sb_swz(sb, KONST(TevKSel::kcsel_odd), "rgb"),
                                     sb_swz(sb, KONST(TevKSel::kasel_odd), "a")))));
#undef KONST
  rslc_b_func_end(sb->b);
}
}  // namespace
}  // namespace UberShader

namespace UberShader
{
namespace
{
/* the leaves of the color and alpha dest trees, and of the blend factor trees */
struct up_dest_ctx
{
  struct rslc_var* s;
  struct rslc_var* value;
};

void up_color_dest_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const struct up_dest_ctx* d = static_cast<const struct up_dest_ctx*>(ctx);
  sb_set(sb, sb_swz(sb, up_reg(sb, d->s, static_cast<int>(i)), "rgb"), up_r(sb, d->value));
}

void up_alpha_dest_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const struct up_dest_ctx* d = static_cast<const struct up_dest_ctx*>(ctx);
  sb_set(sb, sb_swz(sb, up_reg(sb, d->s, static_cast<int>(i)), "a"), up_r(sb, d->value));
}

struct up_blend_ctx
{
  struct rslc_var* dst;      /* blend_src or blend_dst */
  struct rslc_var* initial;  /* initial_ocol0 */
  struct rslc_var* ocol0;
  struct rslc_var* src_color;
  bool is_dst;
  bool alpha;
};

/* float3(v,v,v) of int literals, as the tables write them */
uint32_t up_f3i(const struct shader_build* sb, int v)
{
  return sb_con(sb, sb->f[3], 3, sb_i(sb, v), sb_i(sb, v), sb_i(sb, v));
}

void up_blend_leaf(const struct up_build* u, u32 i, const void* ctx)
{
  const struct shader_build* sb = u->sb;
  const struct up_blend_ctx* b = static_cast<const struct up_blend_ctx*>(ctx);
  const char* c = b->alpha ? "a" : "rgb";
  const char* aa = b->alpha ? "a" : "aaa";
  bool inv = (i & 1) != 0;
  uint32_t e;
  if (i < 2)  // ZERO, ONE
  {
    e = b->alpha ? sb_f(sb, i ? 1.0 : 0.0) : up_f3i(sb, static_cast<int>(i));
    inv = false;
  }
  else if (i < 4)  // DSTCLR, INVDSTCLR; SRCCLR, INVSRCCLR
    e = sb_swz(sb, up_r(sb, b->is_dst ? b->ocol0 : b->initial), c);
  else if (i < 6)  // SRCALPHA, INVSRCALPHA
    e = sb_swz(sb, up_r(sb, b->src_color), aa);
  else  // DSTALPHA, INVDSTALPHA
    e = sb_swz(sb, up_r(sb, b->initial), aa);
  if (inv)
    e = sb_bin(sb, RSLC_B_SUB, b->alpha ? sb_f(sb, 1.0) : up_f3i(sb, 1), e);
  sb_set(sb, sb_swz(sb, up_r(sb, b->dst), c), e);
}

/* LookupIndirectTexture: indcoord from the indirect stage bt, in a block of its own */
void up_lookup_indirect(const struct up_build* u, struct rslc_var* indcoord, struct rslc_var* bt,
                        struct rslc_var* layer, struct rslc_var* const* fixpoint_uv)
{
  const struct shader_build* sb = u->sb;
  struct rslc_var *iref, *texcoord, *texmap, *uv;
  uint32_t a[9];
  u32 i;
  // in_index_name is the indirect stage, not the tev stage
  // bpmem_iref is packed differently from RAS1_IREF
  // This function assumes bpmem_iref is nonzero (i.e. matrix is not off, and the
  // indirect texture stage is enabled).
  rslc_b_begin_block(sb->b);
  iref = rslc_b_local(sb->b, "iref", sb->u[1], up_pack1(u, up_r(sb, bt), "w"), false);
  texcoord = rslc_b_local(sb->b, "texcoord", sb->u[1], up_bfe_raw(u, up_r(sb, iref), 0, 3), false);
  texmap = rslc_b_local(sb->b, "texmap", sb->u[1], up_bfe_raw(u, up_r(sb, iref), 8, 3), false);
  a[0] = up_r(sb, texcoord);
  for (i = 0; i < u->numtexgen; i++)
    a[i + 1] = up_r(sb, fixpoint_uv[i]);
  uv = rslc_b_local(sb->b, "fixedPoint_uv", sb->i[2],
                    rslc_b_call(sb->b, u->select_tex_coord, a, u->numtexgen + 1), false);
#define INDSCALE(c)                                                                               \
  sb_swz(sb,                                                                                       \
         sb_idx(sb, UP_PSU(PSU_INDTEXSCALE), sb_bin(sb, RSLC_B_SHR, up_r(sb, bt), sb_i(sb, 1))),  \
         c)
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_bin(sb, RSLC_B_EQ,
                               sb_bin(sb, RSLC_B_BAND, up_r(sb, bt), sb_u(sb, 1)), sb_u(sb, 0)),
                        up_set_stmt(sb, up_r(sb, uv),
                                    sb_bin(sb, RSLC_B_SHR, up_r(sb, uv), INDSCALE("xy"))),
                        up_set_stmt(sb, up_r(sb, uv),
                                    sb_bin(sb, RSLC_B_SHR, up_r(sb, uv), INDSCALE("zw")))));
#undef INDSCALE
  sb_set(sb, up_r(sb, indcoord),
         sb_swz(sb,
                up_call(sb, u->sample_wrapper, 3, up_r(sb, texmap), up_r(sb, uv),
                        up_r(sb, layer)),
                "abg"));
  rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
}

/* indcoord.c = (indcoord.c >> shift) + ((bias & bit) != 0u ? one : 0); for one format */
void up_ind_format(const struct up_build* u, struct rslc_var* indcoord, struct rslc_var* bias,
                   struct rslc_var* s, int shift, int one, enum rslc_b_binop bump_op, int bump)
{
  static const char* const comps[3] = {"x", "y", "z"};
  const struct shader_build* sb = u->sb;
  u32 k;
  for (k = 0; k < 3; k++)
  {
    uint32_t c = sb_swz(sb, up_r(sb, indcoord), comps[k]);
    uint32_t sel = rslc_b_select(
        sb->b,
        sb_bin(sb, RSLC_B_NE,
               sb_bin(sb, RSLC_B_BAND, up_r(sb, bias), sb_u(sb, 1u << k)), sb_u(sb, 0)),
        up_int(sb, one), sb_i(sb, 0));
    if (shift)
      c = sb_bin(sb, RSLC_B_SHR, c, sb_i(sb, shift));
    sb_set(sb, sb_swz(sb, up_r(sb, indcoord), comps[k]), sb_bin(sb, RSLC_B_ADD, c, sel));
  }
  sb_set(sb, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP),
         sb_bin(sb, bump_op, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP), sb_i(sb, bump)));
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
}

/* The stage's texturing: texcoords, indirect, the sample */
void up_texturing(const struct up_build* u, struct rslc_var* s, struct rslc_var* ss,
                  struct rslc_var* stage, struct rslc_var* tevcoord, struct rslc_var* layer)
{
  const struct shader_build* sb = u->sb;
  struct rslc_var* fixpoint_uv[8];
  struct rslc_var *tex_coord, *uv, *texture_enabled, *tevind, *bs, *fmt, *bias, *bt;
  struct rslc_var *matrix_index, *matrix_id, *indtevtrans, *sw, *tw, *wrapped;
  uint32_t a[9], then_s, else_s, block;
  char name[16];
  u32 i;

  rslc_b_begin_block(sb->b);
  for (i = 0; i < u->numtexgen; i++)
  {
    const uint32_t texz = sb_swz(sb, pi_ref(&u->in, u->plain.tex0 + static_cast<int>(i)), "z");
    uint32_t sel = rslc_b_select(
        sb->b, sb_bin(sb, RSLC_B_EQ, texz, sb_f(sb, 0.0)),
        sb_swz(sb, pi_ref(&u->in, u->plain.tex0 + static_cast<int>(i)), "xy"),
        sb_bin(sb, RSLC_B_DIV, sb_swz(sb, pi_ref(&u->in, u->plain.tex0 + static_cast<int>(i)), "xy"),
               sb_swz(sb, pi_ref(&u->in, u->plain.tex0 + static_cast<int>(i)), "z")));
    uint32_t dims = sb_con(
        sb, sb->f[2], 1,
        sb_bin(sb, RSLC_B_MUL,
               sb_swz(sb, sb_idx(sb, UP_PSU(PSU_TEXDIMS), sb_i(sb, static_cast<int>(i))), "zw"),
               sb_i(sb, 128)));
    snprintf(name, sizeof(name), "fixpoint_uv%u", i);
    fixpoint_uv[i] = rslc_b_local(sb->b, name, sb->i[2],
                                  sb_con(sb, sb->i[2], 1, sb_bin(sb, RSLC_B_MUL, sel, dims)), false);
    // TODO: S24 overflows here?
  }
  tex_coord = rslc_b_local(
      sb->b, "tex_coord", sb->u[1],
      UP_BFE(TwoTevStageOrders::texcoord_even, sb_mem(sb, up_r(sb, ss), SS_ORDER)), false);
  a[0] = up_r(sb, tex_coord);
  for (i = 0; i < u->numtexgen; i++)
    a[i + 1] = up_r(sb, fixpoint_uv[i]);
  uv = rslc_b_local(sb->b, "fixedPoint_uv", sb->i[2],
                    rslc_b_call(sb->b, u->select_tex_coord, a, u->numtexgen + 1), false);
  texture_enabled = rslc_b_local(
      sb->b, "texture_enabled", sb->bl,
      sb_bin(sb, RSLC_B_NE,
             sb_bin(sb, RSLC_B_BAND, sb_mem(sb, up_r(sb, ss), SS_ORDER),
                    sb_u(sb, 1u << TwoTevStageOrders().enable_tex_even.StartBit())),
             sb_u(sb, 0)),
      false);

  // Indirect textures
  tevind = rslc_b_local(sb->b, "tevind", sb->u[1], up_pack1(u, up_r(sb, stage), "z"), false);
  rslc_b_begin_block(sb->b);
  bs = rslc_b_local(sb->b, "bs", sb->u[1], UP_BFE(TevStageIndirect::bs, up_r(sb, tevind)), false);
  fmt = rslc_b_local(sb->b, "fmt", sb->u[1], UP_BFE(TevStageIndirect::fmt, up_r(sb, tevind)),
                     false);
  bias = rslc_b_local(sb->b, "bias", sb->u[1], UP_BFE(TevStageIndirect::bias, up_r(sb, tevind)),
                      false);
  bt = rslc_b_local(sb->b, "bt", sb->u[1], UP_BFE(TevStageIndirect::bt, up_r(sb, tevind)), false);
  matrix_index =
      rslc_b_local(sb->b, "matrix_index", sb->u[1],
                   UP_BFE(TevStageIndirect::matrix_index, up_r(sb, tevind)), false);
  matrix_id = rslc_b_local(sb->b, "matrix_id", sb->u[1],
                           UP_BFE(TevStageIndirect::matrix_id, up_r(sb, tevind)), false);
  indtevtrans = rslc_b_local(sb->b, "indtevtrans", sb->i[2], up_ivec(sb, 2, 0), false);
  // There is always a bit set in bpmem_iref if the data is valid (matrix is not off, and the
  // indirect texture stage is enabled). If the matrix is off, the result doesn't matter; if the
  // indirect texture stage is disabled, the result is undefined (and produces a glitchy pattern
  // on hardware, different from this).
  // For the undefined case, we just skip applying the indirect operation, which is close
  // enough. Wrapping and add to previous still apply in this case (and when the stage is
  // disabled).
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var* indcoord = rslc_b_local(sb->b, "indcoord", sb->i[3], 0, false);
    struct rslc_var *mtxidx, *shift;
    uint32_t body, mtx_then;
    up_lookup_indirect(u, indcoord, bt, layer, fixpoint_uv);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, up_r(sb, bs), sb_u(sb, 0)),
                          up_set_stmt(sb, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP),
                                      sb_idx(sb, up_r(sb, indcoord),
                                             sb_bin(sb, RSLC_B_SUB, up_r(sb, bs), sb_u(sb, 1)))),
                          0));
    rslc_b_begin_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(IndTexFormat::ITF_8))));
    up_ind_format(u, indcoord, bias, s, 0, -128, RSLC_B_BAND, 0xf8);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(IndTexFormat::ITF_5))));
    up_ind_format(u, indcoord, bias, s, 3, 1, RSLC_B_SHL, 5);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(IndTexFormat::ITF_4))));
    up_ind_format(u, indcoord, bias, s, 4, 1, RSLC_B_SHL, 4);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(IndTexFormat::ITF_3))));
    up_ind_format(u, indcoord, bias, s, 5, 1, RSLC_B_SHL, 3);
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, fmt), body));

    // Matrix multiply
    rslc_b_begin_block(sb->b);
    mtxidx = rslc_b_local(
        sb->b, "mtxidx", sb->u[1],
        sb_bin(sb, RSLC_B_MUL, sb_u(sb, 2),
               sb_bin(sb, RSLC_B_SUB, up_r(sb, matrix_index), sb_u(sb, 1))),
        false);
    shift = rslc_b_local(
        sb->b, "shift", sb->i[1],
        sb_swz(sb, sb_idx(sb, UP_PSU(PSU_INDTEXMTX), up_r(sb, mtxidx)), "w"), false);
    rslc_b_begin_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, 0));  // 3x2 S0.10 matrix
    {
      uint32_t d0[2], d1[2];
      d0[0] = sb_swz(sb, sb_idx(sb, UP_PSU(PSU_INDTEXMTX), up_r(sb, mtxidx)), "xyz");
      d0[1] = up_r(sb, indcoord);
      d1[0] = sb_swz(sb,
                     sb_idx(sb, UP_PSU(PSU_INDTEXMTX),
                            sb_bin(sb, RSLC_B_ADD, up_r(sb, mtxidx), sb_u(sb, 1))),
                     "xyz");
      d1[1] = up_r(sb, indcoord);
      sb_set(sb, up_r(sb, indtevtrans),
             sb_bin(sb, RSLC_B_SHR,
                    sb_con(sb, sb->i[2], 2, rslc_b_call(sb->b, u->c.idot3, d0, 2),
                           rslc_b_call(sb->b, u->c.idot3, d1, 2)),
                    sb_i(sb, 3)));
    }
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, 1));  // S matrix, S17.7 format
    sb_set(sb, up_r(sb, indtevtrans),
           sb_bin(sb, RSLC_B_SHR,
                  sb_bin(sb, RSLC_B_MUL, up_r(sb, uv), sb_swz(sb, up_r(sb, indcoord), "xx")),
                  sb_i(sb, 8)));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, 2));  // T matrix, S17.7 format
    sb_set(sb, up_r(sb, indtevtrans),
           sb_bin(sb, RSLC_B_SHR,
                  sb_bin(sb, RSLC_B_MUL, up_r(sb, uv), sb_swz(sb, up_r(sb, indcoord), "yy")),
                  sb_i(sb, 8)));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, matrix_id), body));
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b, sb_bin(sb, RSLC_B_GE, up_r(sb, shift), sb_i(sb, 0)),
                  up_set_stmt(sb, up_r(sb, indtevtrans),
                              sb_bin(sb, RSLC_B_SHR, up_r(sb, indtevtrans), up_r(sb, shift))),
                  up_set_stmt(sb, up_r(sb, indtevtrans),
                              sb_bin(sb, RSLC_B_SHL, up_r(sb, indtevtrans),
                                     sb_bin(sb, RSLC_B_BAND,
                                            sb_un(sb, RSLC_B_NEG, up_r(sb, shift)),
                                            sb_i(sb, 31))))));
    mtx_then = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                                 sb_bin(sb, RSLC_B_NE, up_r(sb, matrix_index), sb_u(sb, 0)),
                                 mtx_then, 0));
  }
  block = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_NE, up_pack1(u, up_r(sb, bt), "w"), sb_u(sb, 0)),
                               block, 0));

  // Wrapping
  sw = rslc_b_local(sb->b, "sw", sb->u[1], UP_BFE(TevStageIndirect::sw, up_r(sb, tevind)), false);
  tw = rslc_b_local(sb->b, "tw", sb->u[1], UP_BFE(TevStageIndirect::tw, up_r(sb, tevind)), false);
  wrapped = rslc_b_local(
      sb->b, "wrapped_coord", sb->i[2],
      sb_con(sb, sb->i[2], 2,
             up_call(sb, u->wrap, 2, sb_swz(sb, up_r(sb, uv), "x"), up_r(sb, sw)),
             up_call(sb, u->wrap, 2, sb_swz(sb, up_r(sb, uv), "y"), up_r(sb, tw))),
      false);
  // add previous tevcoord
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b,
                sb_bin(sb, RSLC_B_NE,
                       sb_bin(sb, RSLC_B_BAND, up_r(sb, tevind),
                              sb_u(sb, 1u << TevStageIndirect().fb_addprev.StartBit())),
                       sb_u(sb, 0)),
                rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_ADD,
                                                 sb_swz(sb, up_r(sb, tevcoord), "xy"),
                                                 sb_bin(sb, RSLC_B_ADD, up_r(sb, wrapped),
                                                        up_r(sb, indtevtrans)))),
                up_set_stmt(sb, sb_swz(sb, up_r(sb, tevcoord), "xy"),
                            sb_bin(sb, RSLC_B_ADD, up_r(sb, wrapped), up_r(sb, indtevtrans)))));
  // Emulate s24 overflows
  sb_set(sb, sb_swz(sb, up_r(sb, tevcoord), "xy"),
         sb_bin(sb, RSLC_B_SHR,
                sb_bin(sb, RSLC_B_SHL, sb_swz(sb, up_r(sb, tevcoord), "xy"), sb_i(sb, 8)),
                sb_i(sb, 8)));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  sb_set(sb, sb_swz(sb, up_r(sb, tevcoord), "xy"), up_r(sb, uv));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, up_r(sb, tevind), sb_u(sb, 0)),
                               then_s, else_s));

  // Sample texture for stage
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var *sampler_num, *swap;
    sampler_num = rslc_b_local(
        sb->b, "sampler_num", sb->u[1],
        UP_BFE(TwoTevStageOrders::texmap_even, sb_mem(sb, up_r(sb, ss), SS_ORDER)), false);
    sb_set(sb, sb_mem(sb, up_r(sb, s), ST_RAW_TEX_COLOR),
           up_call(sb, u->sample_wrapper, 3, up_r(sb, sampler_num),
                   sb_swz(sb, up_r(sb, tevcoord), "xy"), up_r(sb, layer)));
    swap = rslc_b_local(
        sb->b, "swap", sb->u[1],
        UP_BFE(TevStageCombiner::AlphaCombiner::tswap, sb_mem(sb, up_r(sb, ss), SS_AC)), false);
    sb_set(sb, sb_mem(sb, up_r(sb, s), ST_TEX_COLOR),
           up_call(sb, u->swizzle, 2, up_r(sb, swap),
                   sb_mem(sb, up_r(sb, s), ST_RAW_TEX_COLOR)));
  }
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  // Texture is disabled
  sb_set(sb, sb_mem(sb, up_r(sb, s), ST_TEX_COLOR), up_ivec(sb, 4, 255));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, up_r(sb, texture_enabled), then_s, else_s));

  block = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_NE, UP_PSU(PSU_NUMTEXGEN), sb_u(sb, 0)), block,
                               0));
}

/* One combiner's inputs, lerp or compare, clamp and dest: alpha when alpha */
void up_combiner(const struct up_build* u, struct rslc_var* s, struct rslc_var* ss,
                 const struct sb_access* colors, struct rslc_var** color_ab, bool alpha)
{
  const struct shader_build* sb = u->sb;
  const struct rslc_type* t = alpha ? sb->i[1] : sb->i[3];
  const char* pre = alpha ? "alpha_" : "color_";
  struct rslc_var *in[4], *bias, *op, *clamp_v, *scale, *dest, *compare_op, *v[4], *res;
  struct up_dest_ctx dctx;
  char name[24];
  u32 k;

#define CC(field) UP_BFE(TevStageCombiner::ColorCombiner::field, sb_mem(sb, up_r(sb, ss), SS_CC))
#define AC(field) UP_BFE(TevStageCombiner::AlphaCombiner::field, sb_mem(sb, up_r(sb, ss), SS_AC))
#define FIELD(field) (alpha ? AC(field) : CC(field))
#define NAME(n) (snprintf(name, sizeof(name), "%s%s", pre, n), name)
  in[0] = rslc_b_local(sb->b, NAME("a"), sb->u[1], FIELD(a), false);
  in[1] = rslc_b_local(sb->b, NAME("b"), sb->u[1], FIELD(b), false);
  in[2] = rslc_b_local(sb->b, NAME("c"), sb->u[1], FIELD(c), false);
  in[3] = rslc_b_local(sb->b, NAME("d"), sb->u[1], FIELD(d), false);
  bias = rslc_b_local(sb->b, NAME("bias"), sb->u[1], FIELD(bias), false);
  op = rslc_b_local(sb->b, NAME("op"), sb->bl, sb_con(sb, sb->bl, 1, FIELD(op)), false);
  clamp_v = rslc_b_local(sb->b, NAME("clamp"), sb->bl, sb_con(sb, sb->bl, 1, FIELD(clamp)), false);
  scale = rslc_b_local(sb->b, NAME("scale"), sb->u[1], FIELD(scale), false);
  dest = rslc_b_local(sb->b, NAME("dest"), sb->u[1], FIELD(dest), false);
  compare_op = rslc_b_local(sb->b, NAME("compare_op"), sb->u[1],
                            sb_bin(sb, RSLC_B_BOR,
                                   sb_bin(sb, RSLC_B_SHL, up_r(sb, scale), sb_i(sb, 1)),
                                   sb_con(sb, sb->u[1], 1, up_r(sb, op))),
                            false);
#undef CC
#undef AC
#undef FIELD

#define SELECT(k)                                                                                 \
  up_call(sb, alpha ? u->select_alpha_input : u->select_color_input, 5, up_r(sb, s),              \
          up_r(sb, ss), sb_access_ref(sb, &colors[0]), sb_access_ref(sb, &colors[1]),            \
          up_r(sb, in[k]))
#define MASK(e) sb_bin(sb, RSLC_B_BAND, e, alpha ? sb_i(sb, 255) : up_ivec3(sb, 255))
  if (alpha)
  {
    uint32_t then_s;
    v[0] = rslc_b_local(sb->b, "alpha_A", sb->i[1], sb_i(sb, 0), false);
    v[1] = rslc_b_local(sb->b, "alpha_B", sb->i[1], sb_i(sb, 0), false);
    // Small optimisation here: alpha_A and alpha_B are unused by compare ops 0-5
    rslc_b_begin_block(sb->b);
    sb_set(sb, up_r(sb, v[0]), MASK(SELECT(0)));
    sb_set(sb, up_r(sb, v[1]), MASK(SELECT(1)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_OR,
                                 sb_bin(sb, RSLC_B_NE, up_r(sb, bias), sb_u(sb, 3)),
                                 sb_bin(sb, RSLC_B_GT, up_r(sb, compare_op), sb_u(sb, 5))),
                          then_s, 0));
    v[2] = rslc_b_local(sb->b, "alpha_C", sb->i[1], MASK(SELECT(2)), false);
    v[3] = rslc_b_local(sb->b, "alpha_D", sb->i[1], SELECT(3), false);  // 10 bits + sign
  }
  else
  {
    v[0] = rslc_b_local(sb->b, "color_A", t, MASK(SELECT(0)), false);
    v[1] = rslc_b_local(sb->b, "color_B", t, MASK(SELECT(1)), false);
    v[2] = rslc_b_local(sb->b, "color_C", t, MASK(SELECT(2)), false);
    v[3] = rslc_b_local(sb->b, "color_D", t, SELECT(3), false);  // 10 bits + sign
    color_ab[0] = v[0];
    color_ab[1] = v[1];
  }
#undef SELECT
#undef MASK

  res = rslc_b_local(sb->b, alpha ? "alpha" : "color", t, 0, false);
  {
    uint32_t normal_s, cmp_s, e, sel6, sel7, other;
    // Normal mode
    rslc_b_begin_block(sb->b);
    e = up_call(sb, u->tev_lerp[alpha ? 0 : 1], 7, up_r(sb, v[0]), up_r(sb, v[1]), up_r(sb, v[2]),
                up_r(sb, v[3]), up_r(sb, bias), up_r(sb, op), up_r(sb, scale));
    sb_set(sb, up_r(sb, res), e);
    normal_s = rslc_b_end_block(sb->b);
    // Compare mode: op 6 and 7 do a select per channel
    rslc_b_begin_block(sb->b);
    for (k = 0; k < 2; k++)
    {
      static const char* const comps[3] = {"r", "g", "b"};
      const enum rslc_b_binop cmp = k ? RSLC_B_EQ : RSLC_B_GT;
      uint32_t* out = k ? &sel7 : &sel6;
      u32 c;
      rslc_b_begin_block(sb->b);
      if (alpha)
      {
        sb_set(sb, up_r(sb, res),
               rslc_b_select(sb->b, sb_bin(sb, cmp, up_r(sb, v[0]), up_r(sb, v[1])),
                             up_r(sb, v[2]), sb_i(sb, 0)));
      }
      else
      {
        for (c = 0; c < 3; c++)
          sb_set(sb, sb_swz(sb, up_r(sb, res), comps[c]),
                 rslc_b_select(sb->b,
                               sb_bin(sb, cmp, sb_swz(sb, up_r(sb, v[0]), comps[c]),
                                      sb_swz(sb, up_r(sb, v[1]), comps[c])),
                               sb_swz(sb, up_r(sb, v[2]), comps[c]), sb_i(sb, 0)));
      }
      *out = rslc_b_end_block(sb->b);
    }
    // The remaining ops do one compare which selects all channels; all remaining alpha
    // compare ops actually compare the color channels
    rslc_b_begin_block(sb->b);
    sb_set(sb, up_r(sb, res),
           rslc_b_select(sb->b,
                         up_call(sb, u->tev_compare, 3, up_r(sb, compare_op),
                                 up_r(sb, color_ab[0]), up_r(sb, color_ab[1])),
                         up_r(sb, v[2]), alpha ? sb_i(sb, 0) : up_ivec(sb, 3, 0)));
    other = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, up_r(sb, compare_op), sb_u(sb, 6)), sel6,
                          rslc_b_if(sb->b,
                                    sb_bin(sb, RSLC_B_EQ, up_r(sb, compare_op), sb_u(sb, 7)),
                                    sel7, other)));
    sb_set(sb, up_r(sb, res), sb_bin(sb, RSLC_B_ADD, up_r(sb, v[3]), up_r(sb, res)));
    cmp_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, up_r(sb, bias), sb_u(sb, 3)),
                                 normal_s, cmp_s));
  }
  // Clamp result
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, up_r(sb, clamp_v),
                        up_set_stmt(sb, up_r(sb, res),
                                    sb_call(sb, "clamp", 3, up_r(sb, res), sb_i(sb, 0),
                                            sb_i(sb, 255))),
                        up_set_stmt(sb, up_r(sb, res),
                                    sb_call(sb, "clamp", 3, up_r(sb, res), up_int(sb, -1024),
                                            sb_i(sb, 1023)))));
  // Write result to the correct input register of the next stage
  dctx.s = s;
  dctx.value = res;
  up_switch_tree(u, dest, 0, 4, alpha ? up_alpha_dest_leaf : up_color_dest_leaf, &dctx);
#undef NAME
}
}  // namespace
}  // namespace UberShader

namespace UberShader
{
namespace
{
/* discard_fragment: discard, or with broken discard under early depth the framebuffer's own
 * value written back */
uint32_t up_discard(const struct up_build* u, bool fetch_discard, struct rslc_var* real_ocol0,
                    struct rslc_var* initial)
{
  const struct shader_build* sb = u->sb;
  if (!fetch_discard)
    return rslc_b_discard(sb->b);
  rslc_b_begin_block(sb->b);
  sb_set(sb, up_r(sb, real_ocol0),
         sb_con(sb, sb->f[4], 2, sb_swz(sb, up_r(sb, initial), "xyz"), sb_f(sb, 1.0)));
  up_return(sb, 0);
  return rslc_b_end_block(sb->b);
}

/* The fog, as the pixel shader's but read from the uniforms */
void up_fog(const struct up_build* u, struct rslc_var* rawpos, struct rslc_var* zcoord,
            struct rslc_var* tev_result)
{
  const struct shader_build* sb = u->sb;
  struct rslc_var *fog_function, *ze, *fog, *ifog;
  uint32_t then_s, else_s, body;

#define FOGF(c) sb_swz(sb, UP_PSU(PSU_FOGF), c)
#define FOGI(c) sb_swz(sb, UP_PSU(PSU_FOGI), c)
  fog_function = rslc_b_local(sb->b, "fog_function", sb->u[1],
                              UP_BFE(FogParam3::fsel, UP_PSU(PSU_FOGPARAM3)), false);
  rslc_b_begin_block(sb->b);
  // TODO: This all needs to be converted from float to fixed point
  ze = rslc_b_local(sb->b, "ze", sb->f[1], 0, false);
  // perspective: ze = A/(B - (Zs >> B_SHF)
  rslc_b_begin_block(sb->b);
  sb_set(sb, up_r(sb, ze),
         sb_bin(sb, RSLC_B_DIV, sb_bin(sb, RSLC_B_MUL, FOGF("x"), sb_f(sb, 16777216.0)),
                sb_con(sb, sb->f[1], 1,
                       sb_bin(sb, RSLC_B_SUB, FOGI("y"),
                              sb_bin(sb, RSLC_B_SHR, up_r(sb, zcoord), FOGI("w"))))));
  then_s = rslc_b_end_block(sb->b);
  // orthographic: ze = a*Zs (here, no B_SHF)
  rslc_b_begin_block(sb->b);
  sb_set(sb, up_r(sb, ze),
         sb_bin(sb, RSLC_B_DIV,
                sb_bin(sb, RSLC_B_MUL, FOGF("x"), sb_con(sb, sb->f[1], 1, up_r(sb, zcoord))),
                sb_f(sb, 16777216.0)));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_bin(sb, RSLC_B_EQ, UP_BFE(FogParam3::proj, UP_PSU(PSU_FOGPARAM3)),
                               sb_u(sb, 0)),
                        then_s, else_s));

  // x_adjust = sqrt((x-center)^2 + k^2)/k; ze *= x_adjust
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var *offset, *floatindex, *lower, *upper, *klower, *kupper, *k, *x_adjust;
    uint32_t a[3];
#define FOGRANGE(i)                                                                               \
  sb_idx(sb,                                                                                       \
         sb_idx(sb, UP_PSU(PSU_FOGRANGE), sb_bin(sb, RSLC_B_SHR, up_r(sb, i), sb_u(sb, 2))),     \
         sb_bin(sb, RSLC_B_BAND, up_r(sb, i), sb_u(sb, 3)))
    offset = rslc_b_local(
        sb->b, "offset", sb->f[1],
        sb_bin(sb, RSLC_B_SUB,
               sb_bin(sb, RSLC_B_SUB,
                      sb_bin(sb, RSLC_B_MUL, sb_f(sb, 2.0),
                             sb_bin(sb, RSLC_B_DIV, sb_swz(sb, up_r(sb, rawpos), "x"),
                                    FOGF("w"))),
                      sb_f(sb, 1.0)),
               FOGF("z")),
        false);
    floatindex = rslc_b_local(
        sb->b, "floatindex", sb->f[1],
        sb_call(sb, "clamp", 3,
                sb_bin(sb, RSLC_B_SUB, sb_f(sb, 9.0),
                       sb_bin(sb, RSLC_B_MUL, sb_call(sb, "abs", 1, up_r(sb, offset)),
                              sb_f(sb, 9.0))),
                sb_f(sb, 0.0), sb_f(sb, 9.0)),
        false);
    lower = rslc_b_local(sb->b, "indexlower", sb->u[1],
                         sb_con(sb, sb->u[1], 1, up_r(sb, floatindex)), false);
    upper = rslc_b_local(sb->b, "indexupper", sb->u[1],
                         sb_bin(sb, RSLC_B_ADD, up_r(sb, lower), sb_u(sb, 1)), false);
    klower = rslc_b_local(sb->b, "klower", sb->f[1], FOGRANGE(lower), false);
    kupper = rslc_b_local(sb->b, "kupper", sb->f[1], FOGRANGE(upper), false);
    a[0] = up_r(sb, klower);
    a[1] = up_r(sb, kupper);
    a[2] = sb_call(sb, "fract", 1, up_r(sb, floatindex));
    k = rslc_b_local(sb->b, "k", sb->f[1], rslc_b_builtin(sb->b, "mix", a, 3), false);
    x_adjust = rslc_b_local(
        sb->b, "x_adjust", sb->f[1],
        sb_bin(sb, RSLC_B_DIV,
               sb_call(sb, "sqrt", 1,
                       sb_bin(sb, RSLC_B_ADD,
                              sb_bin(sb, RSLC_B_MUL, up_r(sb, offset), up_r(sb, offset)),
                              sb_bin(sb, RSLC_B_MUL, up_r(sb, k), up_r(sb, k)))),
               up_r(sb, k)),
        false);
    sb_op_set(sb, RSLC_B_MUL, up_r(sb, ze), up_r(sb, x_adjust));
#undef FOGRANGE
  }
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_con(sb, sb->bl, 1,
                               UP_BFE(FogRangeParams::RangeBase::Enabled,
                                      UP_PSU(PSU_FOGRANGEBASE))),
                        then_s, 0));

  fog = rslc_b_local(sb->b, "fog", sb->f[1],
                     sb_call(sb, "clamp", 3, sb_bin(sb, RSLC_B_SUB, up_r(sb, ze), FOGF("y")),
                             sb_f(sb, 0.0), sb_f(sb, 1.0)),
                     false);
#define NEG8() sb_un(sb, RSLC_B_NEG, sb_f(sb, 8.0))
  rslc_b_begin_block(sb->b);
  rslc_b_begin_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(FogType::Exp))));
  sb_set(sb, up_r(sb, fog),
         sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0),
                sb_call(sb, "exp2", 1, sb_bin(sb, RSLC_B_MUL, NEG8(), up_r(sb, fog)))));
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(FogType::ExpSq))));
  sb_set(sb, up_r(sb, fog),
         sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0),
                sb_call(sb, "exp2", 1,
                        sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_MUL, NEG8(), up_r(sb, fog)),
                               up_r(sb, fog)))));
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
  rslc_b_stmt(sb->b,
              rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(FogType::BackwardsExp))));
  sb_set(sb, up_r(sb, fog),
         sb_call(sb, "exp2", 1,
                 sb_bin(sb, RSLC_B_MUL, NEG8(),
                        sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), up_r(sb, fog)))));
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
  rslc_b_stmt(sb->b,
              rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(FogType::BackwardsExpSq))));
  sb_set(sb, up_r(sb, fog), sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), up_r(sb, fog)));
  sb_set(sb, up_r(sb, fog),
         sb_call(sb, "exp2", 1,
                 sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_MUL, NEG8(), up_r(sb, fog)),
                        up_r(sb, fog))));
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, up_r(sb, fog_function), body));
  then_s = rslc_b_end_block(sb->b);
#undef NEG8
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_GE, up_r(sb, fog_function),
                                      sb_u(sb, ShaderEnumValue(FogType::Exp))),
                               then_s, 0));
  {
    uint32_t e = sb_bin(sb, RSLC_B_MUL, up_r(sb, fog), sb_f(sb, 256.0));
    ifog = rslc_b_local(sb->b, "ifog", sb->i[1], rslc_b_call(sb->b, u->c.iround[1], &e, 1),
                        false);
  }
  sb_set(sb, sb_swz(sb, up_r(sb, tev_result), "rgb"),
         sb_bin(sb, RSLC_B_SHR,
                sb_bin(sb, RSLC_B_ADD,
                       sb_bin(sb, RSLC_B_MUL, sb_swz(sb, up_r(sb, tev_result), "rgb"),
                              sb_bin(sb, RSLC_B_SUB, sb_i(sb, 256), up_r(sb, ifog))),
                       sb_bin(sb, RSLC_B_MUL, sb_swz(sb, UP_PSU(PSU_FOGCOLOR), "rgb"),
                              up_r(sb, ifog))),
                sb_i(sb, 8)));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_NE, up_r(sb, fog_function),
                                      sb_u(sb, ShaderEnumValue(FogType::Off))),
                               then_s, 0));
#undef FOGF
#undef FOGI
}
}  // namespace

bool BuildPixelShader(struct shader_build* sb, const void* arg)
{
  const PixelShaderBuildArgs* a = static_cast<const PixelShaderBuildArgs*>(arg);
  const ShaderHostConfig& host_config = *a->host_config;
  const pixel_ubershader_uid_data* uid_data = a->uid_data;
  const bool per_pixel_lighting = host_config.per_pixel_lighting;
  const bool stereo = host_config.stereo;
  const bool use_framebuffer_fetch = host_config.backend_shader_framebuffer_fetch;
  const bool use_dual_source = host_config.backend_dual_source_blend && !uid_data->no_dual_src;
  const bool early_depth = uid_data->early_depth != 0;
  const bool per_pixel_depth = uid_data->per_pixel_depth != 0;
  const bool bounding_box = host_config.bounding_box;
  struct up_build ub;
  struct up_build* u = &ub;
  struct rslc_b_quals q;
  struct rslc_func* calculate_lighting = nullptr;
  struct rslc_func* fmain;
  struct rslc_var *ocol0 = nullptr, *ocol1 = nullptr, *real_ocol0 = nullptr, *in_ocol0 = nullptr;
  struct rslc_var *layer_in, *rawpos, *num_stages, *initial = nullptr, *tevcoord, *s;
  struct rslc_var *tev_result, *zcoord, *early_zcoord;
  struct sb_access colors[2];
  struct rslc_var* color_ab[2];
  const struct rslc_type* out_t;
  uint32_t then_s, else_s;
  int k;

  memset(u, 0, sizeof(*u));
  u->sb = sb;
  u->host_config = &host_config;
  u->uid_data = uid_data;
  u->api_type = a->api_type;
  u->numtexgen = uid_data->num_texgens;

  BuildBitfieldExtractHeader(sb, host_config, &u->bitfield);
  BuildPixelShaderCommonHeader(sb, host_config, bounding_box, &u->bitfield, &u->c);
  if (per_pixel_lighting)
    calculate_lighting = BuildLightingFunction(sb, u->c.vs[SU_LIGHTS]);

  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.location = sb_output_location(sb, 0);
#ifdef __APPLE__
  // Framebuffer fetch is only supported by Metal, so ensure that we're running Vulkan (MoltenVK)
  // if we want to use it.
  if (a->api_type == APIType::Vulkan || a->api_type == APIType::Metal)
  {
    if (use_dual_source)
    {
      q.index = 0;
      ocol0 = rslc_b_declare(sb->b, &q, "ocol0", sb->f[4], 0);
      q.index = 1;
      ocol1 = rslc_b_declare(sb->b, &q, "ocol1", sb->f[4], 0);
    }
    else if (use_framebuffer_fetch)
    {
      // Metal doesn't support a single unified variable for both input and output, so when
      // using framebuffer fetch, we declare the input separately below.
      real_ocol0 = rslc_b_declare(sb->b, &q, "real_ocol0", sb->f[4], 0);
    }
    else
    {
      ocol0 = rslc_b_declare(sb->b, &q, "ocol0", sb->f[4], 0);
    }
  }
  else
#endif
  {
    out_t = uid_data->uint_output ? sb->u[4] : sb->f[4];
    if (use_framebuffer_fetch)
    {
      real_ocol0 = rslc_b_declare(sb->b, &q, "real_ocol0", sb->f[4], 0);
    }
    else
    {
      q.index = 0;
      ocol0 = rslc_b_declare(sb->b, &q, "ocol0", out_t, 0);
    }
    if (use_dual_source)
    {
      q.index = 1;
      ocol1 = rslc_b_declare(sb->b, &q, "ocol1", out_t, 0);
    }
  }
  if (use_framebuffer_fetch)
  {
    // The colour attachment's value on entry: Metal's framebuffer fetch, or an OpenGL context's
    // framebuffer fetch extension
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_UNIFORM;
    q.input_attachment = 0;
    if (sb->api != SHADER_BUILD_OPENGL)
    {
      q.set = 0;
      q.binding = 0;
    }
    in_ocol0 = rslc_b_declare(sb->b, &q, "in_ocol0",
                              rslc_b_sampler_type(sb->b, RSLC_B_SUBPASS_INPUT, RSLC_BASE_FLOAT),
                              0);
  }

  BuildPixelShaderInputs(sb, host_config, u->numtexgen, &u->in, &u->plain, &layer_in);

  // Uniform index -> texture coordinates
  // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord does
  // not exist), then tex coord 0 is used (though sometimes glitchy effects happen on console).
  if (u->numtexgen > 0)
    up_select_functions(u);
  up_sample_wrapper(u);
  up_swizzle(u);
  up_wrap(u);
  u->tev_lerp[0] = up_tev_lerp(u, 1);
  u->tev_lerp[1] = up_tev_lerp(u, 3);
  up_tev_compare(u);
  up_input_selects(u);

  if (early_depth && host_config.backend_early_z)
  {
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.early_fragment_tests = true;
    rslc_b_layout(sb->b, &q);
  }

  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);
  rawpos = rslc_b_local(sb->b, "rawpos", sb->f[4], sb_bi(sb, "gl_FragCoord"), false);
  num_stages = rslc_b_local(sb->b, "num_stages", sb->u[1],
                            UP_BFE(GenMode::numtevstages, UP_PSU(PSU_GENMODE)), false);
  if (use_framebuffer_fetch)
  {
    // Store off a copy of the initial framebuffer value.
    initial = rslc_b_local(sb->b, "initial_ocol0", sb->f[4],
                           sb_call(sb, "subpassLoad", 1, up_r(sb, in_ocol0)), false);
    // QComm's Adreno driver doesn't seem to like using the framebuffer_fetch value as an
    // intermediate value with multiple reads & modifications, so we pull out the "real" output
    // value above and use a temporary for calculations, then set the output value once at the
    // end of the shader.
    ocol0 = rslc_b_local(sb->b, "ocol0", sb->f[4], 0, false);
    ocol1 = rslc_b_local(sb->b, "ocol1", sb->f[4], 0, false);
  }
  if (host_config.backend_geometry_shaders && stereo)
  {
    if (host_config.backend_gl_layer_in_fs)
      layer_in = rslc_b_local(sb->b, "layer", sb->i[1], sb_bi(sb, "gl_Layer"), false);
  }
  else
  {
    layer_in = rslc_b_local(sb->b, "layer", sb->i[1], sb_i(sb, 0), false);
  }

  tevcoord = rslc_b_local(sb->b, "tevcoord", sb->i[3], up_ivec(sb, 3, 0), false);
  s = rslc_b_local(sb->b, "s", u->state_t, 0, false);
  sb_set(sb, sb_mem(sb, up_r(sb, s), ST_TEX_COLOR), up_ivec(sb, 4, 0));
  sb_set(sb, sb_mem(sb, up_r(sb, s), ST_RAW_TEX_COLOR), up_ivec(sb, 4, 0));
  sb_set(sb, sb_mem(sb, up_r(sb, s), ST_ALPHA_BUMP), sb_i(sb, 0));
  for (k = 0; k < 4; k++)
    sb_set(sb, up_reg(sb, s, k), sb_idx(sb, UP_PSU(PSU_COLORS), sb_i(sb, k)));

  colors[0].var = u->in.block ? u->in.block : u->in.var[u->plain.colors_0];
  colors[0].member = u->in.block ? u->plain.colors_0 : -1;
  colors[0].swizzle = nullptr;
  colors[1].var = u->in.block ? u->in.block : u->in.var[u->plain.colors_1];
  colors[1].member = u->in.block ? u->plain.colors_1 : -1;
  colors[1].swizzle = nullptr;
  if (per_pixel_lighting)
  {
    struct VertexLightingBuild l;
    struct rslc_var *lit0, *lit1, *lit_normal, *lit_pos;
    lit0 = rslc_b_local(sb->b, "lit_colors_0", sb->f[4], sb_access_ref(sb, &colors[0]), false);
    lit1 = rslc_b_local(sb->b, "lit_colors_1", sb->f[4], sb_access_ref(sb, &colors[1]), false);
    lit_normal = rslc_b_local(
        sb->b, "lit_normal", sb->f[3],
        sb_call(sb, "normalize", 1, sb_swz(sb, pi_ref(&u->in, u->plain.Normal), "xyz")), false);
    lit_pos = rslc_b_local(sb->b, "lit_pos", sb->f[3],
                           sb_swz(sb, pi_ref(&u->in, u->plain.WorldPos), "xyz"), false);
    memset(&l, 0, sizeof(l));
    l.uniforms = u->c.vs;
    l.bitfield = &u->bitfield;
    l.calculate_lighting = calculate_lighting;
    l.world_pos.var = lit_pos;
    l.world_pos.member = -1;
    l.normal.var = lit_normal;
    l.normal.member = -1;
    l.in_color[0] = colors[0];
    l.in_color[1] = colors[1];
    l.out_color[0].var = lit0;
    l.out_color[0].member = -1;
    l.out_color[1].var = lit1;
    l.out_color[1].member = -1;
    BuildVertexLighting(sb, &l);
    // The number of colors available to TEV is determined by numColorChans. Normally this is
    // performed in the vertex shader after lighting, but with per-pixel lighting, we need to
    // perform it here. TODO: Actually implement this for ubershaders
    colors[0].var = lit0;
    colors[0].member = -1;
    colors[1].var = lit1;
    colors[1].member = -1;
  }

  // Main tev loop
  {
    struct rslc_var *stage, *ss;
    uint32_t init, body;
    init = rslc_b_decl_stmt(sb->b, "stage", sb->u[1], sb_u(sb, 0), &stage);
    rslc_b_begin_block(sb->b);
    ss = rslc_b_local(sb->b, "ss", u->stage_state_t, 0, false);
    sb_set(sb, sb_mem(sb, up_r(sb, ss), SS_STAGE), up_r(sb, stage));
    sb_set(sb, sb_mem(sb, up_r(sb, ss), SS_CC),
           sb_swz(sb, up_pack1(u, up_r(sb, stage), "xy"), "x"));
    sb_set(sb, sb_mem(sb, up_r(sb, ss), SS_AC),
           sb_swz(sb, up_pack1(u, up_r(sb, stage), "xy"), "y"));
    sb_set(sb, sb_mem(sb, up_r(sb, ss), SS_ORDER),
           up_pack2(u, sb_bin(sb, RSLC_B_SHR, up_r(sb, stage), sb_i(sb, 1)), "x"));
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_EQ,
                                 sb_bin(sb, RSLC_B_BAND, up_r(sb, stage), sb_u(sb, 1)),
                                 sb_u(sb, 1)),
                          up_set_stmt(sb, sb_mem(sb, up_r(sb, ss), SS_ORDER),
                                      sb_bin(sb, RSLC_B_SHR, sb_mem(sb, up_r(sb, ss), SS_ORDER),
                                             sb_i(sb, int(TwoTevStageOrders().enable_tex_odd.StartBit() -
                                                          TwoTevStageOrders().enable_tex_even.StartBit())))),
                          0));
    // Disable texturing when there are no texgens (for now): those the draw has, where the
    // shader is sized for more
    if (u->numtexgen != 0)
      up_texturing(u, s, ss, stage, tevcoord, layer_in);

    // This is the Meat of TEV
    rslc_b_begin_block(sb->b);
    up_combiner(u, s, ss, colors, color_ab, false);
    up_combiner(u, s, ss, colors, color_ab, true);
    rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_for(sb->b, init,
                           sb_bin(sb, RSLC_B_LE, up_r(sb, stage), up_r(sb, num_stages)),
                           sb_un(sb, RSLC_B_POSTINC, up_r(sb, stage)), body));
  }

  // Select the output color and alpha registers from the last stage.
  tev_result = rslc_b_local(sb->b, "TevResult", sb->i[4], 0, false);
  sb_set(sb, sb_swz(sb, up_r(sb, tev_result), "xyz"),
         sb_swz(sb,
                up_call(sb, u->get_tev_reg, 2, up_r(sb, s),
                        UP_BFE(TevStageCombiner::ColorCombiner::dest,
                               sb_swz(sb, up_pack1(u, up_r(sb, num_stages), "xy"), "x"))),
                "xyz"));
  sb_set(sb, sb_swz(sb, up_r(sb, tev_result), "w"),
         sb_swz(sb,
                up_call(sb, u->get_tev_reg, 2, up_r(sb, s),
                        UP_BFE(TevStageCombiner::AlphaCombiner::dest,
                               sb_swz(sb, up_pack1(u, up_r(sb, num_stages), "xy"), "y"))),
                "w"));
  sb_op_set(sb, RSLC_B_BAND, up_r(sb, tev_result), sb_i(sb, 255));

#define ZBIAS(i, c) sb_swz(sb, sb_idx(sb, UP_PSU(PSU_ZBIAS), sb_i(sb, i)), c)
  if (host_config.fast_depth_calc)
  {
    uint32_t z = sb_swz(sb, up_r(sb, rawpos), "z");
    if (!host_config.backend_reversed_depth_range)
      z = sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), z);
    zcoord = rslc_b_local(
        sb->b, "zCoord", sb->i[1],
        sb_con(sb, sb->i[1], 1, sb_bin(sb, RSLC_B_MUL, z, sb_f(sb, 16777216.0))), false);
    sb_set(sb, up_r(sb, zcoord),
           sb_call(sb, "clamp", 3, up_r(sb, zcoord), sb_i(sb, 0), sb_i(sb, 0xFFFFFF)));
  }
  else
  {
    zcoord = rslc_b_local(
        sb->b, "zCoord", sb->i[1],
        sb_bin(sb, RSLC_B_ADD, ZBIAS(1, "x"),
               sb_con(sb, sb->i[1], 1,
                      sb_bin(sb, RSLC_B_MUL,
                             sb_bin(sb, RSLC_B_DIV,
                                    sb_swz(sb, pi_ref(&u->in, u->plain.clipPos), "z"),
                                    sb_swz(sb, pi_ref(&u->in, u->plain.clipPos), "w")),
                             sb_con(sb, sb->f[1], 1, ZBIAS(1, "y"))))),
        false);
  }

  // ZFreeze forces early depth off
  if (per_pixel_depth)
  {
    struct rslc_var* screenpos;
    rslc_b_begin_block(sb->b);
    screenpos = rslc_b_local(sb->b, "screenpos", sb->f[2],
                             sb_bin(sb, RSLC_B_MUL, sb_swz(sb, up_r(sb, rawpos), "xy"),
                                    sb_swz(sb, UP_PSU(PSU_EFBSCALE), "xy")),
                             false);
    // OpenGL has reversed vertical screenspace coordinates
    if (a->api_type == APIType::OpenGL)
      sb_set(sb, sb_swz(sb, up_r(sb, screenpos), "y"),
             sb_bin(sb, RSLC_B_SUB, sb_f(sb, 528.0), sb_swz(sb, up_r(sb, screenpos), "y")));
    sb_set(sb, up_r(sb, zcoord),
           sb_con(sb, sb->i[1], 1,
                  sb_bin(sb, RSLC_B_ADD,
                         sb_bin(sb, RSLC_B_ADD, sb_swz(sb, UP_PSU(PSU_ZSLOPE), "z"),
                                sb_bin(sb, RSLC_B_MUL, sb_swz(sb, UP_PSU(PSU_ZSLOPE), "x"),
                                       sb_swz(sb, up_r(sb, screenpos), "x"))),
                         sb_bin(sb, RSLC_B_MUL, sb_swz(sb, UP_PSU(PSU_ZSLOPE), "y"),
                                sb_swz(sb, up_r(sb, screenpos), "y")))));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_NE,
                                 sb_bin(sb, RSLC_B_BAND, UP_PSU(PSU_GENMODE),
                                        sb_u(sb, 1u << GenMode().zfreeze.StartBit())),
                                 sb_u(sb, 0)),
                          then_s, 0));
  }

  // Depth Texture
  early_zcoord = rslc_b_local(sb->b, "early_zCoord", sb->i[1], up_r(sb, zcoord), false);
  {
    struct rslc_var* ztex;
    uint32_t d[2];
    rslc_b_begin_block(sb->b);
    ztex = rslc_b_local(sb->b, "ztex", sb->i[1], sb_con(sb, sb->i[1], 1, ZBIAS(1, "w")),
                        false);  // fixed bias
    // Whatever texture was in our last stage, it's now our depth texture
    d[0] = sb_swz(sb, sb_mem(sb, up_r(sb, s), ST_RAW_TEX_COLOR), "xyzw");
    d[1] = ZBIAS(0, "xyzw");
    sb_op_set(sb, RSLC_B_ADD, up_r(sb, ztex), rslc_b_call(sb->b, u->c.idot4, d, 2));
    sb_op_set(sb, RSLC_B_ADD, up_r(sb, ztex),
              rslc_b_select(sb->b,
                            sb_bin(sb, RSLC_B_EQ, UP_PSU(PSU_ZTEX_OP), sb_u(sb, 1)),
                            up_r(sb, zcoord), sb_i(sb, 0)));
    sb_set(sb, up_r(sb, zcoord), sb_bin(sb, RSLC_B_BAND, up_r(sb, ztex), sb_i(sb, 0xFFFFFF)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, UP_PSU(PSU_ZTEX_OP), sb_u(sb, 0)),
                                 then_s, 0));
  }
#undef ZBIAS

  if (per_pixel_depth)
  {
    // If early depth is enabled, write to zbuffer before depth textures
    // If early depth isn't enabled, we write to the zbuffer here
    struct rslc_var* zbuf = rslc_b_local(
        sb->b, "zbuffer_zCoord", sb->i[1],
        rslc_b_select(sb->b, UP_PSU(PSU_LATE_ZTEST), up_r(sb, zcoord), up_r(sb, early_zcoord)),
        false);
    uint32_t d = sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[1], 1, up_r(sb, zbuf)),
                        sb_f(sb, 16777216.0));
    if (!host_config.backend_reversed_depth_range)
      d = sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), d);
    sb_set(sb, sb_bi(sb, "gl_FragDepth"), d);
  }

  // Alpha Test
  {
    const bool fetch_discard = early_depth &&
                               DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DISCARD_WITH_EARLY_Z) &&
                               host_config.backend_shader_framebuffer_fetch;
    static const enum rslc_b_binop logic[4] = {RSLC_B_AND, RSLC_B_OR, RSLC_B_NE, RSLC_B_EQ};
    struct rslc_var *comp0, *comp1;
    uint32_t body;
    rslc_b_begin_block(sb->b);
    comp0 = rslc_b_local(sb->b, "comp0", sb->bl,
                         up_call(sb, u->alpha_compare, 3, sb_swz(sb, up_r(sb, tev_result), "a"),
                                 sb_swz(sb, UP_PSU(PSU_ALPHA), "r"),
                                 UP_BFE(AlphaTest::comp0, UP_PSU(PSU_ALPHATEST))),
                         false);
    comp1 = rslc_b_local(sb->b, "comp1", sb->bl,
                         up_call(sb, u->alpha_compare, 3, sb_swz(sb, up_r(sb, tev_result), "a"),
                                 sb_swz(sb, UP_PSU(PSU_ALPHA), "g"),
                                 UP_BFE(AlphaTest::comp1, UP_PSU(PSU_ALPHATEST))),
                         false);
    // These if statements are written weirdly to work around intel and Qualcomm bugs with
    // handling booleans.
    rslc_b_begin_block(sb->b);
    for (k = 0; k < 4; k++)  // AND, OR, XOR, XNOR
    {
      rslc_b_stmt(sb->b, rslc_b_case(sb->b, k));
      rslc_b_stmt(sb->b,
                  rslc_b_if(sb->b, sb_bin(sb, logic[k], up_r(sb, comp0), up_r(sb, comp1)),
                            rslc_b_break(sb->b),
                            up_discard(u, fetch_discard, real_ocol0, initial)));
      rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    }
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, UP_BFE(AlphaTest::logic, UP_PSU(PSU_ALPHATEST)),
                                     body));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, UP_PSU(PSU_ALPHATEST), sb_u(sb, 0)),
                                 then_s, 0));
  }

  // Hardware testing indicates that an alpha of 1 can pass an alpha test,
  // but doesn't do anything in blending
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_bin(sb, RSLC_B_EQ, sb_swz(sb, up_r(sb, tev_result), "a"), sb_i(sb, 1)),
                        up_set_stmt(sb, sb_swz(sb, up_r(sb, tev_result), "a"), sb_i(sb, 0)), 0));

  // Flipper uses a standard 2x2 Bayer Matrix for 6 bit dithering
  {
    struct rslc_var* dither;
    rslc_b_begin_block(sb->b);
    dither = rslc_b_local(
        sb->b, "dither", sb->i[2],
        sb_bin(sb, RSLC_B_BAND, sb_con(sb, sb->i[2], 1, sb_swz(sb, up_r(sb, rawpos), "xy")),
               sb_i(sb, 1)),
        false);
    sb_set(sb, sb_swz(sb, up_r(sb, tev_result), "rgb"),
           sb_bin(sb, RSLC_B_ADD,
                  sb_bin(sb, RSLC_B_ADD,
                         sb_bin(sb, RSLC_B_SUB, sb_swz(sb, up_r(sb, tev_result), "rgb"),
                                sb_bin(sb, RSLC_B_SHR, sb_swz(sb, up_r(sb, tev_result), "rgb"),
                                       sb_i(sb, 6))),
                         sb_bin(sb, RSLC_B_MUL,
                                sb_bin(sb, RSLC_B_BXOR, sb_swz(sb, up_r(sb, dither), "x"),
                                       sb_swz(sb, up_r(sb, dither), "y")),
                                sb_i(sb, 2))),
                  sb_swz(sb, up_r(sb, dither), "y")));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, UP_PSU(PSU_DITHER), then_s, 0));
  }

  // FIXME: Fog is implemented the same as ShaderGen, but ShaderGen's fog is all hacks.
  //        Should be fixed point, and should not make guesses about Range-Based adjustments.
  up_fog(u, rawpos, zcoord, tev_result);

  if (use_framebuffer_fetch)
  {
    struct rslc_var* fb;
    uint32_t body, e;
    // Logic Ops
    rslc_b_begin_block(sb->b);
    e = sb_bin(sb, RSLC_B_MUL, up_r(sb, initial), sb_f(sb, 255.0));
    fb = rslc_b_local(sb->b, "fb_value", sb->i[4], rslc_b_call(sb->b, u->c.iround[4], &e, 1),
                      false);
    rslc_b_begin_block(sb->b);
    for (k = 0; k < 16; k++)
    {
#define T() up_r(sb, tev_result)
#define F() up_r(sb, fb)
#define NOT(x) sb_un(sb, RSLC_B_BITNOT, x)
      uint32_t v;
      switch (k)
      {
      case 0: v = up_ivec(sb, 4, 0); break;                                  // CLEAR
      case 1: v = sb_bin(sb, RSLC_B_BAND, T(), F()); break;                  // AND
      case 2: v = sb_bin(sb, RSLC_B_BAND, T(), NOT(F())); break;             // AND_REVERSE
      case 3: v = T(); break;                                                // COPY
      case 4: v = sb_bin(sb, RSLC_B_BAND, NOT(T()), F()); break;             // AND_INVERTED
      case 5: v = F(); break;                                                // NOOP
      case 6: v = sb_bin(sb, RSLC_B_BXOR, T(), F()); break;                  // XOR
      case 7: v = sb_bin(sb, RSLC_B_BOR, T(), F()); break;                   // OR
      case 8: v = NOT(sb_bin(sb, RSLC_B_BOR, T(), F())); break;              // NOR
      case 9: v = NOT(sb_bin(sb, RSLC_B_BXOR, T(), F())); break;             // EQUIV
      case 10: v = NOT(F()); break;                                          // INVERT
      case 11: v = sb_bin(sb, RSLC_B_BOR, T(), NOT(F())); break;             // OR_REVERSE
      case 12: v = NOT(T()); break;                                          // COPY_INVERTED
      case 13: v = sb_bin(sb, RSLC_B_BOR, NOT(T()), F()); break;             // OR_INVERTED
      case 14: v = NOT(sb_bin(sb, RSLC_B_BAND, T(), F())); break;            // NAND
      default: v = up_ivec(sb, 4, 255); break;                               // SET
      }
#undef T
#undef F
#undef NOT
      rslc_b_stmt(sb->b, rslc_b_case(sb->b, k));
      sb_set(sb, up_r(sb, tev_result), v);
      rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    }
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, UP_PSU(PSU_LOGIC_OP_MODE), body));
    sb_op_set(sb, RSLC_B_BAND, up_r(sb, tev_result), sb_i(sb, 0xff));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, UP_PSU(PSU_LOGIC_OP_ENABLE), then_s, 0));
  }
  else if (!host_config.backend_logic_op)
  {
    uint32_t body;
    // Helpers for logic op blending approximations
    rslc_b_begin_block(sb->b);
    rslc_b_begin_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(LogicOp::Clear)));
    sb_set(sb, up_r(sb, tev_result), up_ivec(sb, 4, 0));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(LogicOp::CopyInverted)));
    sb_op_set(sb, RSLC_B_BXOR, up_r(sb, tev_result), sb_i(sb, 0xff));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(LogicOp::Set)));
    rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(LogicOp::Invert)));
    sb_set(sb, up_r(sb, tev_result), up_ivec(sb, 4, 255));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    rslc_b_stmt(sb->b, rslc_b_default(sb->b));
    rslc_b_stmt(sb->b, rslc_b_break(sb->b));
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, UP_PSU(PSU_LOGIC_OP_MODE), body));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, UP_PSU(PSU_LOGIC_OP_ENABLE), then_s, 0));
  }

  // Some backends require that the shader outputs be uint when writing to a uint render target
  // for logic op.
  if (uid_data->uint_output)
  {
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, UP_PSU(PSU_RGBA6_FORMAT),
                          up_set_stmt(sb, up_r(sb, ocol0),
                                      sb_con(sb, sb->u[4], 1,
                                             sb_bin(sb, RSLC_B_BAND, up_r(sb, tev_result),
                                                    sb_i(sb, 0xFC)))),
                          up_set_stmt(sb, up_r(sb, ocol0),
                                      sb_con(sb, sb->u[4], 1, up_r(sb, tev_result)))));
  }
  else
  {
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b, UP_PSU(PSU_RGBA6_FORMAT),
                  up_set_stmt(sb, sb_swz(sb, up_r(sb, ocol0), "rgb"),
                              sb_bin(sb, RSLC_B_DIV,
                                     sb_con(sb, sb->f[3], 1,
                                            sb_bin(sb, RSLC_B_SHR,
                                                   sb_swz(sb, up_r(sb, tev_result), "rgb"),
                                                   sb_i(sb, 2))),
                                     sb_f(sb, 63.0))),
                  up_set_stmt(sb, sb_swz(sb, up_r(sb, ocol0), "rgb"),
                              sb_bin(sb, RSLC_B_DIV,
                                     sb_con(sb, sb->f[3], 1,
                                            sb_swz(sb, up_r(sb, tev_result), "rgb")),
                                     sb_f(sb, 255.0)))));
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, UP_PSU(PSU_DSTALPHA), sb_u(sb, 0)),
                  up_set_stmt(sb, sb_swz(sb, up_r(sb, ocol0), "a"),
                              sb_bin(sb, RSLC_B_DIV,
                                     sb_con(sb, sb->f[1], 1,
                                            sb_bin(sb, RSLC_B_SHR,
                                                   UP_BFE(ConstantAlpha::alpha,
                                                          UP_PSU(PSU_DSTALPHA)),
                                                   sb_i(sb, 2))),
                                     sb_f(sb, 63.0))),
                  up_set_stmt(sb, sb_swz(sb, up_r(sb, ocol0), "a"),
                              sb_bin(sb, RSLC_B_DIV,
                                     sb_con(sb, sb->f[1], 1,
                                            sb_bin(sb, RSLC_B_SHR,
                                                   sb_swz(sb, up_r(sb, tev_result), "a"),
                                                   sb_i(sb, 2))),
                                     sb_f(sb, 63.0)))));
    if (use_dual_source || use_framebuffer_fetch)
    {
      // Dest alpha override (dual source blending)
      // Colors will be blended against the alpha from ocol1 and
      // the alpha from ocol0 will be written to the framebuffer.
      sb_set(sb, up_r(sb, ocol1),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0),
                    sb_bin(sb, RSLC_B_DIV,
                           sb_con(sb, sb->f[1], 1, sb_swz(sb, up_r(sb, tev_result), "a")),
                           sb_f(sb, 255.0))));
    }
  }

  if (bounding_box)
  {
    rslc_b_begin_block(sb->b);
    sb_do(sb, up_call(sb, u->c.update_bounding_box, 1, sb_swz(sb, up_r(sb, rawpos), "xy")));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, UP_PSU(PSU_BOUNDING_BOX), then_s, 0));
  }

  if (use_framebuffer_fetch)
  {
    struct up_blend_ctx bc;
    struct rslc_var *src_color, *blend_src, *blend_dst, *blend_result;
    rslc_b_begin_block(sb->b);
    src_color = rslc_b_local(sb->b, "src_color", sb->f[4], 0, false);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, UP_PSU(PSU_DSTALPHA), sb_u(sb, 0)),
                          up_set_block(sb, up_r(sb, src_color), up_r(sb, ocol1)),
                          up_set_block(sb, up_r(sb, src_color), up_r(sb, ocol0))));
    blend_src = rslc_b_local(sb->b, "blend_src", sb->f[4], 0, false);
    bc.initial = initial;
    bc.ocol0 = ocol0;
    bc.src_color = src_color;
    bc.dst = blend_src;
    bc.is_dst = false;
    bc.alpha = false;
    up_switch_tree(u, u->c.ps[PSU_BLEND_SRC_FACTOR], 0, 8, up_blend_leaf, &bc);
    bc.alpha = true;
    up_switch_tree(u, u->c.ps[PSU_BLEND_SRC_FACTOR_ALPHA], 0, 8, up_blend_leaf, &bc);
    blend_dst = rslc_b_local(sb->b, "blend_dst", sb->f[4], 0, false);
    bc.dst = blend_dst;
    bc.is_dst = true;
    bc.alpha = false;
    up_switch_tree(u, u->c.ps[PSU_BLEND_DST_FACTOR], 0, 8, up_blend_leaf, &bc);
    bc.alpha = true;
    up_switch_tree(u, u->c.ps[PSU_BLEND_DST_FACTOR_ALPHA], 0, 8, up_blend_leaf, &bc);
    blend_result = rslc_b_local(sb->b, "blend_result", sb->f[4], 0, false);
    for (k = 0; k < 2; k++)
    {
      const char* c = k ? "a" : "rgb";
      uint32_t parts[2];
      int j;
      for (j = 0; j < 2; j++)
        parts[j] = sb_bin(
            sb, j ? RSLC_B_SUB : RSLC_B_ADD,
            sb_bin(sb, RSLC_B_MUL, sb_swz(sb, up_r(sb, initial), c),
                   sb_swz(sb, up_r(sb, blend_dst), c)),
            sb_bin(sb, RSLC_B_MUL, sb_swz(sb, up_r(sb, ocol0), c),
                   sb_swz(sb, up_r(sb, blend_src), c)));
      rslc_b_stmt(sb->b,
                  rslc_b_if(sb->b, UP_PSU(k ? PSU_BLEND_SUBTRACT_ALPHA : PSU_BLEND_SUBTRACT),
                            up_set_stmt(sb, sb_swz(sb, up_r(sb, blend_result), c), parts[1]),
                            up_set_stmt(sb, sb_swz(sb, up_r(sb, blend_result), c), parts[0])));
    }
    sb_set(sb, up_r(sb, real_ocol0), up_r(sb, blend_result));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    sb_set(sb, up_r(sb, real_ocol0), up_r(sb, ocol0));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, UP_PSU(PSU_BLEND_ENABLE), then_s, else_s));
  }
  if (!rslc_b_func_end(sb->b))
    return false;

  up_ras_konst_bodies(u);
  return true;
}
}  // namespace UberShader
