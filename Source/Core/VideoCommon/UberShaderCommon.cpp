// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/UberShaderCommon.h"

#include <utility>

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
namespace
{
enum
{
  LIGHT_COLOR,
  LIGHT_COSATT,
  LIGHT_DISTATT,
  LIGHT_POS,
  LIGHT_DIR
};

/* clights[index].field */
uint32_t ul_light(const struct shader_build* sb, struct rslc_var* lights, struct rslc_var* index,
                  int field)
{
  return sb_mem(sb, sb_idx(sb, sb_ref(sb, lights), sb_ref(sb, index)),
                static_cast<uint32_t>(field));
}

/* case value: as a statement of the open block */
void ul_case(const struct shader_build* sb, u32 value)
{
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(value)));
}

void ul_break(const struct shader_build* sb)
{
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
}

/* int4(round(e)) returned */
void ul_return_round(const struct shader_build* sb, uint32_t e)
{
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b, sb_con(sb, sb->i[4], 1, sb_call(sb, "round", 1, e))));
}

/* float3(1.0, a, a*a) */
uint32_t ul_poly(const struct shader_build* sb, struct rslc_var* a)
{
  return sb_con(sb, sb->f[3], 3, sb_f(sb, 1.0), sb_ref(sb, a),
                sb_bin(sb, RSLC_B_MUL, sb_ref(sb, a), sb_ref(sb, a)));
}

/* (chan == 0u) ? c0.s : c1.s */
uint32_t ul_pick_color(const struct shader_build* sb, const struct VertexLightingBuild* l,
                       struct rslc_var* chan, const char* s)
{
  return rslc_b_select(sb->b, sb_bin(sb, RSLC_B_EQ, sb_ref(sb, chan), sb_u(sb, 0u)),
                       sb_swz(sb, sb_access_ref(sb, &l->in_color[0]), s),
                       sb_swz(sb, sb_access_ref(sb, &l->in_color[1]), s));
}

/* int(round(e * 255.0)) of width n */
uint32_t ul_round255(const struct shader_build* sb, unsigned n, uint32_t e)
{
  return sb_con(sb, sb->i[n], 1,
                sb_call(sb, "round", 1, sb_bin(sb, RSLC_B_MUL, e, sb_f(sb, 255.0))));
}

/* if (e != 0u) */
uint32_t ul_nonzero(const struct shader_build* sb, uint32_t e)
{
  return sb_bin(sb, RSLC_B_NE, e, sb_u(sb, 0u));
}

/* cmtrl[chan + k] */
uint32_t ul_material(const struct shader_build* sb, const struct VertexLightingBuild* l,
                     struct rslc_var* chan, u32 k)
{
  uint32_t idx = sb_ref(sb, chan);
  if (k)
    idx = sb_bin(sb, RSLC_B_ADD, idx, sb_u(sb, k));
  return sb_idx(sb, sb_ref(sb, l->uniforms[SU_MATERIALS]), idx);
}

/* uint light_mask, attnfunc, diffusefunc of reg; then the loop adding the lights to
 * lacc.swizzle */
void ul_lights(const struct shader_build* sb, const struct VertexLightingBuild* l,
               struct rslc_var* reg, struct rslc_var* lacc, const char* swizzle)
{
  struct rslc_var *light_mask, *attnfunc, *diffusefunc, *light_index;
  uint32_t init, body, args[5];
  light_mask = rslc_b_local(
      sb->b, "light_mask", sb->u[1],
      sb_bin(sb, RSLC_B_BOR,
             BuildBitfieldExtract<&LitChannel::lightMask0_3>(sb, l->bitfield, sb_ref(sb, reg)),
             sb_bin(sb, RSLC_B_SHL,
                    BuildBitfieldExtract<&LitChannel::lightMask4_7>(sb, l->bitfield,
                                                                    sb_ref(sb, reg)),
                    sb_u(sb, 4u))),
      false);
  attnfunc = rslc_b_local(
      sb->b, "attnfunc", sb->u[1],
      BuildBitfieldExtract<&LitChannel::attnfunc>(sb, l->bitfield, sb_ref(sb, reg)), false);
  diffusefunc = rslc_b_local(
      sb->b, "diffusefunc", sb->u[1],
      BuildBitfieldExtract<&LitChannel::diffusefunc>(sb, l->bitfield, sb_ref(sb, reg)), false);
  init = rslc_b_decl_stmt(sb->b, "light_index", sb->u[1], sb_u(sb, 0u), &light_index);
  rslc_b_begin_block(sb->b);
  args[0] = sb_ref(sb, light_index);
  args[1] = sb_ref(sb, attnfunc);
  args[2] = sb_ref(sb, diffusefunc);
  args[3] = sb_access_ref(sb, &l->world_pos);
  args[4] = sb_access_ref(sb, &l->normal);
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b,
                ul_nonzero(sb, sb_bin(sb, RSLC_B_BAND, sb_ref(sb, light_mask),
                                      sb_bin(sb, RSLC_B_SHL, sb_u(sb, 1u),
                                             sb_ref(sb, light_index)))),
                rslc_b_expr(sb->b, rslc_b_assign(
                                       sb->b, RSLC_B_ADD, sb_swz(sb, sb_ref(sb, lacc), swizzle),
                                       sb_swz(sb, rslc_b_call(sb->b, l->calculate_lighting, args, 5),
                                              swizzle))),
                0));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_for(sb->b, init,
                                sb_bin(sb, RSLC_B_LT, sb_ref(sb, light_index), sb_u(sb, 8u)),
                                sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, light_index)), body));
}

/* if (cond) l = r; else l2 = r2; with the else optional */
uint32_t ul_if_set(const struct shader_build* sb, uint32_t cond, uint32_t l, uint32_t r,
                   uint32_t else_stmt)
{
  return rslc_b_if(sb->b, cond, rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r)),
                   else_stmt);
}

uint32_t ul_set_stmt(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  return rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r));
}
}  // namespace

uint32_t sb_access_ref(const struct shader_build* sb, const struct sb_access* a)
{
  uint32_t e = sb_ref(sb, a->var);
  if (a->member >= 0)
    e = sb_mem(sb, e, static_cast<uint32_t>(a->member));
  if (a->swizzle)
    e = sb_swz(sb, e, a->swizzle);
  return e;
}

struct rslc_func* BuildLightingFunction(const struct shader_build* sb, struct rslc_var* lights)
{
  static const char* const names[5] = {"index", "attnfunc", "diffusefunc", "pos", "normal"};
  static const unsigned dirs[5] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* types[5];
  struct rslc_func* f;
  struct rslc_var *index, *attnfunc, *diffusefunc, *pos, *normal;
  struct rslc_var *ldir, *cos_attn, *dist_attn, *dist, *dist2, *attn;
  uint32_t body, e;

  // ==============================================
  // Lighting channel calculation helper
  // ==============================================
  types[0] = types[1] = types[2] = sb->u[1];
  types[3] = types[4] = sb->f[3];
  f = rslc_b_func(sb->b, "CalculateLighting", sb->i[4], 5, types, dirs, names);
  rslc_b_func_begin(sb->b, f);
  index = rslc_b_param(f, 0);
  attnfunc = rslc_b_param(f, 1);
  diffusefunc = rslc_b_param(f, 2);
  pos = rslc_b_param(f, 3);
  normal = rslc_b_param(f, 4);
  ldir = rslc_b_local(sb->b, "ldir", sb->f[3], 0, false);
  rslc_b_local(sb->b, "h", sb->f[3], 0, false);
  cos_attn = rslc_b_local(sb->b, "cosAttn", sb->f[3], 0, false);
  dist_attn = rslc_b_local(sb->b, "distAttn", sb->f[3], 0, false);
  dist = rslc_b_local(sb->b, "dist", sb->f[1], 0, false);
  dist2 = rslc_b_local(sb->b, "dist2", sb->f[1], 0, false);
  attn = rslc_b_local(sb->b, "attn", sb->f[1], 0, false);

#define UL(field) ul_light(sb, lights, index, field)
  rslc_b_begin_block(sb->b);
  ul_case(sb, ShaderEnumValue(AttenuationFunc::None));
  ul_case(sb, ShaderEnumValue(AttenuationFunc::Dir));
  sb_set(sb, sb_ref(sb, ldir),
         sb_call(sb, "normalize", 1,
                 sb_bin(sb, RSLC_B_SUB, sb_swz(sb, UL(LIGHT_POS), "xyz"),
                        sb_swz(sb, sb_ref(sb, pos), "xyz"))));
  sb_set(sb, sb_ref(sb, attn), sb_f(sb, 1.0));
  rslc_b_stmt(sb->b,
              ul_if_set(sb,
                        sb_bin(sb, RSLC_B_EQ, sb_call(sb, "length", 1, sb_ref(sb, ldir)),
                               sb_f(sb, 0.0)),
                        sb_ref(sb, ldir), sb_ref(sb, normal), 0));
  ul_break(sb);

  ul_case(sb, ShaderEnumValue(AttenuationFunc::Spec));
  sb_set(sb, sb_ref(sb, ldir),
         sb_call(sb, "normalize", 1,
                 sb_bin(sb, RSLC_B_SUB, sb_swz(sb, UL(LIGHT_POS), "xyz"),
                        sb_swz(sb, sb_ref(sb, pos), "xyz"))));
  sb_set(sb, sb_ref(sb, attn),
         rslc_b_select(sb->b,
                       sb_bin(sb, RSLC_B_GE,
                              sb_call(sb, "dot", 2, sb_ref(sb, normal), sb_ref(sb, ldir)),
                              sb_f(sb, 0.0)),
                       sb_call(sb, "max", 2, sb_f(sb, 0.0),
                               sb_call(sb, "dot", 2, sb_ref(sb, normal),
                                       sb_swz(sb, UL(LIGHT_DIR), "xyz"))),
                       sb_f(sb, 0.0)));
  sb_set(sb, sb_ref(sb, cos_attn), sb_swz(sb, UL(LIGHT_COSATT), "xyz"));
  rslc_b_stmt(sb->b,
              ul_if_set(sb,
                        sb_bin(sb, RSLC_B_EQ, sb_ref(sb, diffusefunc),
                               sb_u(sb, ShaderEnumValue(DiffuseFunc::None))),
                        sb_ref(sb, dist_attn), sb_swz(sb, UL(LIGHT_DISTATT), "xyz"),
                        ul_set_stmt(sb, sb_ref(sb, dist_attn),
                                    sb_call(sb, "normalize", 1,
                                            sb_swz(sb, UL(LIGHT_DISTATT), "xyz")))));
  sb_set(sb, sb_ref(sb, attn),
         sb_bin(sb, RSLC_B_DIV,
                sb_call(sb, "max", 2, sb_f(sb, 0.0),
                        sb_call(sb, "dot", 2, sb_ref(sb, cos_attn), ul_poly(sb, attn))),
                sb_call(sb, "dot", 2, sb_ref(sb, dist_attn), ul_poly(sb, attn))));
  ul_break(sb);

  ul_case(sb, ShaderEnumValue(AttenuationFunc::Spot));
  sb_set(sb, sb_ref(sb, ldir),
         sb_bin(sb, RSLC_B_SUB, sb_swz(sb, UL(LIGHT_POS), "xyz"),
                sb_swz(sb, sb_ref(sb, pos), "xyz")));
  sb_set(sb, sb_ref(sb, dist2), sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, ldir)));
  sb_set(sb, sb_ref(sb, dist), sb_call(sb, "sqrt", 1, sb_ref(sb, dist2)));
  sb_set(sb, sb_ref(sb, ldir), sb_bin(sb, RSLC_B_DIV, sb_ref(sb, ldir), sb_ref(sb, dist)));
  sb_set(sb, sb_ref(sb, attn),
         sb_call(sb, "max", 2, sb_f(sb, 0.0),
                 sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_swz(sb, UL(LIGHT_DIR), "xyz"))));
  e = sb_bin(sb, RSLC_B_ADD,
             sb_bin(sb, RSLC_B_ADD, sb_swz(sb, UL(LIGHT_COSATT), "x"),
                    sb_bin(sb, RSLC_B_MUL, sb_swz(sb, UL(LIGHT_COSATT), "y"), sb_ref(sb, attn))),
             sb_bin(sb, RSLC_B_MUL,
                    sb_bin(sb, RSLC_B_MUL, sb_swz(sb, UL(LIGHT_COSATT), "z"), sb_ref(sb, attn)),
                    sb_ref(sb, attn)));
  sb_set(sb, sb_ref(sb, attn),
         sb_bin(sb, RSLC_B_DIV, sb_call(sb, "max", 2, sb_f(sb, 0.0), e),
                sb_call(sb, "dot", 2, sb_swz(sb, UL(LIGHT_DISTATT), "xyz"),
                        sb_con(sb, sb->f[3], 3, sb_f(sb, 1.0), sb_ref(sb, dist),
                               sb_ref(sb, dist2)))));
  ul_break(sb);

  rslc_b_stmt(sb->b, rslc_b_default(sb->b));
  sb_set(sb, sb_ref(sb, attn), sb_f(sb, 1.0));
  sb_set(sb, sb_ref(sb, ldir), sb_ref(sb, normal));
  ul_break(sb);
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, attnfunc), body));

  rslc_b_begin_block(sb->b);
  ul_case(sb, ShaderEnumValue(DiffuseFunc::None));
  ul_return_round(sb, sb_bin(sb, RSLC_B_MUL, sb_ref(sb, attn),
                             sb_con(sb, sb->f[4], 1, UL(LIGHT_COLOR))));
  ul_case(sb, ShaderEnumValue(DiffuseFunc::Sign));
  ul_return_round(sb, sb_bin(sb, RSLC_B_MUL,
                             sb_bin(sb, RSLC_B_MUL, sb_ref(sb, attn),
                                    sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, normal))),
                             sb_con(sb, sb->f[4], 1, UL(LIGHT_COLOR))));
  ul_case(sb, ShaderEnumValue(DiffuseFunc::Clamp));
  ul_return_round(sb, sb_bin(sb, RSLC_B_MUL,
                             sb_bin(sb, RSLC_B_MUL, sb_ref(sb, attn),
                                    sb_call(sb, "max", 2, sb_f(sb, 0.0),
                                            sb_call(sb, "dot", 2, sb_ref(sb, ldir),
                                                    sb_ref(sb, normal)))),
                             sb_con(sb, sb->f[4], 1, UL(LIGHT_COLOR))));
  rslc_b_stmt(sb->b, rslc_b_default(sb->b));
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_con(sb, sb->i[4], 4, sb_i(sb, 0), sb_i(sb, 0),
                                                 sb_i(sb, 0), sb_i(sb, 0))));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, diffusefunc), body));
#undef UL
  rslc_b_func_end(sb->b);
  return f;
}

void BuildVertexLighting(const struct shader_build* sb, const struct VertexLightingBuild* l)
{
  struct rslc_var *chan, *colorreg, *alphareg, *mat, *lacc, *lit_color;
  uint32_t init, body, then_s, inner;

  init = rslc_b_decl_stmt(sb->b, "chan", sb->u[1], sb_u(sb, 0u), &chan);
  rslc_b_begin_block(sb->b);
  colorreg = rslc_b_local(
      sb->b, "colorreg", sb->u[1],
      sb_swz(sb, sb_idx(sb, sb_ref(sb, l->uniforms[SU_PACK1]), sb_ref(sb, chan)), "z"), false);
  alphareg = rslc_b_local(
      sb->b, "alphareg", sb->u[1],
      sb_swz(sb, sb_idx(sb, sb_ref(sb, l->uniforms[SU_PACK1]), sb_ref(sb, chan)), "w"), false);
  mat = rslc_b_local(sb->b, "mat", sb->i[4], ul_material(sb, l, chan, 2), false);
  lacc = rslc_b_local(
      sb->b, "lacc", sb->i[4],
      sb_con(sb, sb->i[4], 4, sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255)), false);

#define BFE(field, reg) BuildBitfieldExtract<&LitChannel::field>(sb, l->bitfield, sb_ref(sb, reg))
  rslc_b_stmt(sb->b, ul_if_set(sb, ul_nonzero(sb, BFE(matsource, colorreg)),
                               sb_swz(sb, sb_ref(sb, mat), "xyz"),
                               ul_round255(sb, 3, ul_pick_color(sb, l, chan, "xyz")), 0));
  rslc_b_stmt(sb->b,
              ul_if_set(sb, ul_nonzero(sb, BFE(matsource, alphareg)),
                        sb_swz(sb, sb_ref(sb, mat), "w"),
                        ul_round255(sb, 1, ul_pick_color(sb, l, chan, "w")),
                        ul_set_stmt(sb, sb_swz(sb, sb_ref(sb, mat), "w"),
                                    sb_swz(sb, ul_material(sb, l, chan, 2), "w"))));

  rslc_b_begin_block(sb->b);
  rslc_b_stmt(sb->b,
              ul_if_set(sb, ul_nonzero(sb, BFE(ambsource, colorreg)),
                        sb_swz(sb, sb_ref(sb, lacc), "xyz"),
                        ul_round255(sb, 3, ul_pick_color(sb, l, chan, "xyz")),
                        ul_set_stmt(sb, sb_swz(sb, sb_ref(sb, lacc), "xyz"),
                                    sb_swz(sb, ul_material(sb, l, chan, 0), "xyz"))));
  ul_lights(sb, l, colorreg, lacc, "xyz");
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, ul_nonzero(sb, BFE(enablelighting, colorreg)), then_s, 0));

  rslc_b_begin_block(sb->b);
  rslc_b_begin_block(sb->b);
  inner = ul_if_set(
      sb,
      ul_nonzero(sb, sb_bin(sb, RSLC_B_BAND, sb_ref(sb, l->uniforms[SU_COMPONENTS]),
                            sb_u(sb, std::to_underlying(VB_HAS_COL0)))),
      sb_swz(sb, sb_ref(sb, lacc), "w"),
      ul_round255(sb, 1, sb_swz(sb, sb_access_ref(sb, &l->in_color[0]), "w")),
      ul_set_stmt(sb, sb_swz(sb, sb_ref(sb, lacc), "w"), sb_i(sb, 255)));
  rslc_b_stmt(sb->b,
              ul_if_set(sb,
                        ul_nonzero(sb, sb_bin(sb, RSLC_B_BAND,
                                              sb_ref(sb, l->uniforms[SU_COMPONENTS]),
                                              sb_bin(sb, RSLC_B_SHL,
                                                     sb_u(sb, std::to_underlying(VB_HAS_COL0)),
                                                     sb_ref(sb, chan)))),
                        sb_swz(sb, sb_ref(sb, lacc), "w"),
                        ul_round255(sb, 1, ul_pick_color(sb, l, chan, "w")), inner));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  sb_set(sb, sb_swz(sb, sb_ref(sb, lacc), "w"), sb_swz(sb, ul_material(sb, l, chan, 0), "w"));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, ul_nonzero(sb, BFE(ambsource, alphareg)), then_s, body));
  ul_lights(sb, l, alphareg, lacc, "w");
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, ul_nonzero(sb, BFE(enablelighting, alphareg)), then_s, 0));
#undef BFE

  sb_set(sb, sb_ref(sb, lacc),
         sb_call(sb, "clamp", 3, sb_ref(sb, lacc), sb_i(sb, 0), sb_i(sb, 255)));

  // Hopefully GPUs that can support dynamic indexing will optimize this.
  lit_color = rslc_b_local(
      sb->b, "lit_color", sb->f[4],
      sb_bin(sb, RSLC_B_DIV,
             sb_con(sb, sb->f[4], 1,
                    sb_bin(sb, RSLC_B_SHR,
                           sb_bin(sb, RSLC_B_MUL, sb_ref(sb, mat),
                                  sb_bin(sb, RSLC_B_ADD, sb_ref(sb, lacc),
                                         sb_bin(sb, RSLC_B_SHR, sb_ref(sb, lacc), sb_i(sb, 7)))),
                           sb_i(sb, 8))),
             sb_f(sb, 255.0)),
      false);
  rslc_b_begin_block(sb->b);
  ul_case(sb, 0u);
  sb_set(sb, sb_access_ref(sb, &l->out_color[0]), sb_ref(sb, lit_color));
  ul_break(sb);
  ul_case(sb, 1u);
  sb_set(sb, sb_access_ref(sb, &l->out_color[1]), sb_ref(sb, lit_color));
  ul_break(sb);
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, chan), body));

  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_for(sb->b, init,
                         sb_bin(sb, RSLC_B_LT, sb_ref(sb, chan),
                                sb_u(sb, static_cast<u32>(NUM_XF_COLOR_CHANNELS))),
                         sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, chan)), body));
}
}  // namespace UberShader
