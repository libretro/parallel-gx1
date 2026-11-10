// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/UberShaderVertex.h"

#include <cstring>
#include <utility>

#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/UberShaderCommon.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
VertexShaderUid GetVertexShaderUid()
{
  VertexShaderUid out;

  vertex_ubershader_uid_data* const uid_data = out.GetUidData();
  uid_data->num_texgens = xfmem.numTexGen.numTexGens;

  return out;
}

u32 EnumerateVertexShaderUids(VertexShaderUid* uids)
{
  u32 count = 0;
  u32 texgens;

  for (texgens = 0; texgens <= 8; texgens++)
  {
    uids[count] = VertexShaderUid();
    uids[count].GetUidData()->num_texgens = texgens;
    count++;
  }
  return count;
}
}  // namespace UberShader

/* ---- the stage built by calls ---- */

namespace UberShader
{
namespace
{
struct uv_build
{
  const struct shader_build* sb;
  const ShaderHostConfig* host_config;
  APIType api_type;
  u32 num_texgen;
  bool vertex_loader;
  struct sb_bitfield bitfield;
  struct rslc_var* u[SU_COUNT];
  struct rslc_var* gs[4];
  struct rslc_var* base_vertex;
  struct rslc_var* vertex_buffer;
  struct rslc_var *rawpos, *posmtx, *rawnormal, *rawtangent, *rawbinormal, *rawcolor[2],
      *rawtex[8];
  struct rslc_func *isnan_fn, *calculate_lighting, *base_offset, *load_uint4_ubyte4,
      *load_float4_ubyte4, *load_float3_float3, *load_float4_rawpos, *load_float3_rawtex;
  struct rslc_var *o, *vertex_base_offset, *pos, *normal, *tangent, *binormal;
  struct rslc_var *p[3];
  const struct vs_output_layout* layout;
};

uint32_t uv_u(const struct uv_build* v, int k)
{
  return sb_ref(v->sb, v->u[k]);
}

/* o.member */
uint32_t uv_o(const struct uv_build* v, int member)
{
  return sb_mem(v->sb, sb_ref(v->sb, v->o), static_cast<uint32_t>(member));
}

/* (components & bits) != 0u */
uint32_t uv_has(const struct uv_build* v, u32 bits)
{
  const struct shader_build* sb = v->sb;
  return sb_bin(sb, RSLC_B_NE, sb_bin(sb, RSLC_B_BAND, uv_u(v, SU_COMPONENTS), sb_u(sb, bits)),
                sb_u(sb, 0u));
}

uint32_t uv_stmt_set(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  return rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r));
}

void uv_case(const struct shader_build* sb, u32 value)
{
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(value)));
}

void uv_break(const struct shader_build* sb)
{
  rslc_b_stmt(sb->b, rslc_b_break(sb->b));
}

/* vertex_buffer[offset + k] as a float */
uint32_t uv_word(const struct uv_build* v, struct rslc_var* offset, int k)
{
  const struct shader_build* sb = v->sb;
  return sb_call(sb, "uintBitsToFloat", 1,
                 sb_idx(sb, sb_ref(sb, v->vertex_buffer),
                        sb_bin(sb, RSLC_B_ADD, sb_ref(sb, offset), sb_i(sb, k))));
}

/* the vertex loader's functions, after the storage buffer */
void uv_loader_functions(struct uv_build* v)
{
  static const char* const names[2] = {"vtx_offset", "attr_offset"};
  static const unsigned dirs[2] = {RSLC_B_IN, RSLC_B_IN};
  const struct shader_build* sb = v->sb;
  const struct rslc_type* types[2];
  struct rslc_var *vtx, *attr, *value, *offset, *components;
  uint32_t parts[4], a[2];
  int k;
  types[0] = types[1] = sb->u[1];

  {
    static const char* const vid_names[1] = {"vertex_id"};
    struct rslc_var* vertex_id;
    uint32_t e;
    v->base_offset = rslc_b_func(sb->b, "GetVertexBaseOffset", sb->u[1], 1, types, dirs, vid_names);
    rslc_b_func_begin(sb->b, v->base_offset);
    vertex_id = rslc_b_param(v->base_offset, 0);
    e = sb_ref(sb, vertex_id);
    if (v->api_type == APIType::D3D)
      e = sb_bin(sb, RSLC_B_ADD, e, sb_ref(sb, v->base_vertex));
    rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_bin(sb, RSLC_B_MUL, e,
                                                   uv_u(v, SU_VERTEX_STRIDE))));
    rslc_b_func_end(sb->b);
  }

  v->load_uint4_ubyte4 =
      rslc_b_func(sb->b, "load_input_uint4_ubyte4", sb->u[4], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, v->load_uint4_ubyte4);
  vtx = rslc_b_param(v->load_uint4_ubyte4, 0);
  attr = rslc_b_param(v->load_uint4_ubyte4, 1);
  value = rslc_b_local(sb->b, "value", sb->u[1],
                       sb_idx(sb, sb_ref(sb, v->vertex_buffer),
                              sb_bin(sb, RSLC_B_ADD, sb_ref(sb, vtx), sb_ref(sb, attr))),
                       false);
  parts[0] = sb_bin(sb, RSLC_B_BAND, sb_ref(sb, value), sb_u(sb, 0xffu));
  for (k = 1; k < 3; k++)
    parts[k] = sb_bin(sb, RSLC_B_BAND, sb_bin(sb, RSLC_B_SHR, sb_ref(sb, value), sb_i(sb, 8 * k)),
                      sb_u(sb, 0xffu));
  parts[3] = sb_bin(sb, RSLC_B_SHR, sb_ref(sb, value), sb_i(sb, 24));
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_con(sb, sb->u[4], 4, parts[0], parts[1], parts[2],
                                                 parts[3])));
  rslc_b_func_end(sb->b);

  v->load_float4_ubyte4 =
      rslc_b_func(sb->b, "load_input_float4_ubyte4", sb->f[4], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, v->load_float4_ubyte4);
  a[0] = sb_ref(sb, rslc_b_param(v->load_float4_ubyte4, 0));
  a[1] = sb_ref(sb, rslc_b_param(v->load_float4_ubyte4, 1));
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b, sb_bin(sb, RSLC_B_DIV,
                                          sb_con(sb, sb->f[4], 1,
                                                 rslc_b_call(sb->b, v->load_uint4_ubyte4, a, 2)),
                                          sb_f(sb, 255.0))));
  rslc_b_func_end(sb->b);

  v->load_float3_float3 =
      rslc_b_func(sb->b, "load_input_float3_float3", sb->f[3], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, v->load_float3_float3);
  vtx = rslc_b_param(v->load_float3_float3, 0);
  attr = rslc_b_param(v->load_float3_float3, 1);
  offset = rslc_b_local(sb->b, "offset", sb->u[1],
                        sb_bin(sb, RSLC_B_ADD, sb_ref(sb, vtx), sb_ref(sb, attr)), false);
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_con(sb, sb->f[3], 3, uv_word(v, offset, 0),
                                                 uv_word(v, offset, 1), uv_word(v, offset, 2))));
  rslc_b_func_end(sb->b);

  /* uint components = attr_offset >> 16; uint offset = vtx_offset + (attr_offset & 0xffff); */
#define UV_PACKED_OFFSET(f)                                                                        \
  do                                                                                               \
  {                                                                                                \
    vtx = rslc_b_param(f, 0);                                                                      \
    attr = rslc_b_param(f, 1);                                                                     \
    components = rslc_b_local(sb->b, "components", sb->u[1],                                       \
                              sb_bin(sb, RSLC_B_SHR, sb_ref(sb, attr), sb_i(sb, 16)), false);      \
    offset = rslc_b_local(                                                                         \
        sb->b, "offset", sb->u[1],                                                                 \
        sb_bin(sb, RSLC_B_ADD, sb_ref(sb, vtx),                                                    \
               sb_bin(sb, RSLC_B_BAND, sb_ref(sb, attr), sb_i(sb, 0xffff))),                       \
        false);                                                                                    \
  } while (0)
  v->load_float4_rawpos =
      rslc_b_func(sb->b, "load_input_float4_rawpos", sb->f[4], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, v->load_float4_rawpos);
  UV_PACKED_OFFSET(v->load_float4_rawpos);
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, sb_ref(sb, components), sb_i(sb, 3)),
                rslc_b_return(sb->b, sb_con(sb, sb->f[4], 4, uv_word(v, offset, 0),
                                            uv_word(v, offset, 1), sb_f(sb, 0.0), sb_f(sb, 1.0))),
                rslc_b_return(sb->b, sb_con(sb, sb->f[4], 4, uv_word(v, offset, 0),
                                            uv_word(v, offset, 1), uv_word(v, offset, 2),
                                            sb_f(sb, 1.0)))));
  rslc_b_func_end(sb->b);

  v->load_float3_rawtex =
      rslc_b_func(sb->b, "load_input_float3_rawtex", sb->f[3], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, v->load_float3_rawtex);
  UV_PACKED_OFFSET(v->load_float3_rawtex);
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, sb_ref(sb, components), sb_i(sb, 2)),
                rslc_b_return(sb->b, sb_con(sb, sb->f[3], 3, uv_word(v, offset, 0),
                                            sb_f(sb, 0.0), sb_f(sb, 0.0))),
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_LT, sb_ref(sb, components), sb_i(sb, 3)),
                          rslc_b_return(sb->b, sb_con(sb, sb->f[3], 3, uv_word(v, offset, 0),
                                                      uv_word(v, offset, 1), sb_f(sb, 0.0))),
                          rslc_b_return(sb->b, sb_con(sb, sb->f[3], 3, uv_word(v, offset, 0),
                                                      uv_word(v, offset, 1),
                                                      uv_word(v, offset, 2))))));
  rslc_b_func_end(sb->b);
#undef UV_PACKED_OFFSET
}

/* LoadVertexAttribute: the local the vertex loader reads, or the attribute */
struct rslc_var* uv_load(const struct uv_build* v, const char* name, const struct rslc_type* type,
                         struct rslc_func* load, uint32_t attr_offset, struct rslc_var* attribute)
{
  const struct shader_build* sb = v->sb;
  uint32_t a[2];
  if (!v->vertex_loader)
    return attribute;
  a[0] = sb_ref(sb, v->vertex_base_offset);
  a[1] = attr_offset;
  return rslc_b_local(sb->b, name, type, rslc_b_call(sb->b, load, a, 2), false);
}

/* vertex_offset_rawtex[i / 4][i % 4] */
uint32_t uv_rawtex_offset(const struct uv_build* v, uint32_t i_div, uint32_t i_mod)
{
  return sb_idx(v->sb, sb_idx(v->sb, uv_u(v, SU_OFFSET_RAWTEX), i_div), i_mod);
}

/* output_tex.xyz = float3(dot(coord, m[a]), dot(coord, m[a+1]), dot(coord, m[a+2]) or 1.0) */
void uv_texcoord_rows(const struct uv_build* v, struct rslc_var* output_tex,
                      struct rslc_var* coord, int m, uint32_t (*index)(const struct uv_build*,
                                                                          const void*, int),
                      const void* ctx, bool stq)
{
  const struct shader_build* sb = v->sb;
  uint32_t row[3];
  int k;
  for (k = 0; k < (stq ? 3 : 2); k++)
    row[k] = sb_call(sb, "dot", 2, sb_ref(sb, coord), sb_idx(sb, uv_u(v, m), index(v, ctx, k)));
  if (!stq)
    row[2] = sb_f(sb, 1.0);
  sb_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
         sb_con(sb, sb->f[3], 3, row[0], row[1], row[2]));
}

/* tmp, tmp + 1, tmp + 2 */
uint32_t uv_tmp_index(const struct uv_build* v, const void* ctx, int k)
{
  struct rslc_var* tmp = static_cast<struct rslc_var*>(const_cast<void*>(ctx));
  if (!k)
    return sb_ref(v->sb, tmp);
  return sb_bin(v->sb, RSLC_B_ADD, sb_ref(v->sb, tmp), sb_i(v->sb, k));
}

/* 3u * texgen, 3u * texgen + 1u, 3u * texgen + 2u */
uint32_t uv_texgen_index(const struct uv_build* v, const void* ctx, int k)
{
  struct rslc_var* texgen = static_cast<struct rslc_var*>(const_cast<void*>(ctx));
  uint32_t e = sb_bin(v->sb, RSLC_B_MUL, sb_u(v->sb, 3u), sb_ref(v->sb, texgen));
  if (!k)
    return e;
  return sb_bin(v->sb, RSLC_B_ADD, e, sb_u(v->sb, static_cast<u32>(k)));
}

/* if (BFE(projection) == STQ) { rows of 3 } else { rows of 2, 1.0 } */
void uv_projection(const struct uv_build* v, struct rslc_var* tex_mtx_info,
                   struct rslc_var* output_tex, struct rslc_var* coord, int m,
                   uint32_t (*index)(const struct uv_build*, const void*, int), const void* ctx)
{
  const struct shader_build* sb = v->sb;
  uint32_t then_s, else_s;
  rslc_b_begin_block(sb->b);
  uv_texcoord_rows(v, output_tex, coord, m, index, ctx, true);
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  uv_texcoord_rows(v, output_tex, coord, m, index, ctx, false);
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_bin(sb, RSLC_B_EQ,
                               BuildBitfieldExtract<&TexMtxInfo::projection>(
                                   sb, &v->bitfield, sb_ref(sb, tex_mtx_info)),
                               sb_u(sb, ShaderEnumValue(TexSize::STQ))),
                        then_s, else_s));
}

/* GenVertexShaderTexGens: the body of the texgen loop */
void uv_texgen_body(const struct uv_build* v, struct rslc_var* texgen)
{
  const struct shader_build* sb = v->sb;
  const u32 num_texgen = v->num_texgen;
  struct rslc_var *coord, *tex_mtx_info, *texgentype, *output_tex;
  uint32_t body, then_s, else_s;
  u32 i;

#define BFE(field, var)                                                                            \
  BuildBitfieldExtract<&TexMtxInfo::field>(sb, &v->bitfield, sb_ref(sb, var))
  // Texcoord transforms
  coord = rslc_b_local(
      sb->b, "coord", sb->f[4],
      sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 1.0), sb_f(sb, 1.0)), false);
  tex_mtx_info = rslc_b_local(
      sb->b, "texMtxInfo", sb->u[1],
      sb_swz(sb, sb_idx(sb, uv_u(v, SU_PACK1), sb_ref(sb, texgen)), "x"), false);

  rslc_b_begin_block(sb->b);
  uv_case(sb, ShaderEnumValue(SourceRow::Geom));
  sb_set(sb, sb_swz(sb, sb_ref(sb, coord), "xyz"), sb_swz(sb, sb_ref(sb, v->rawpos), "xyz"));
  uv_break(sb);
  {
    static const SourceRow rows[3] = {SourceRow::Normal, SourceRow::BinormalT,
                                      SourceRow::BinormalB};
    static const u32 bits[3] = {VB_HAS_NORMAL, VB_HAS_TANGENT, VB_HAS_BINORMAL};
    static const char* const names[3] = {"rawnormal", "rawtangent", "rawbinormal"};
    static const int offsets[3] = {SU_OFFSET_RAWNORMAL, SU_OFFSET_RAWTANGENT,
                                   SU_OFFSET_RAWBINORMAL};
    struct rslc_var* const attributes[3] = {v->rawnormal, v->rawtangent, v->rawbinormal};
    int k;
    for (k = 0; k < 3; k++)
    {
      struct rslc_var* raw;
      uv_case(sb, ShaderEnumValue(rows[k]));
      rslc_b_begin_block(sb->b);
      raw = uv_load(v, names[k], sb->f[3], v->load_float3_float3, uv_u(v, offsets[k]),
                    attributes[k]);
      sb_set(sb, sb_swz(sb, sb_ref(sb, coord), "xyz"), sb_swz(sb, sb_ref(sb, raw), "xyz"));
      then_s = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_if(sb->b, uv_has(v, bits[k]), then_s, 0));
      uv_break(sb);
    }
  }
  for (i = 0; i < 8; i++)
  {
    static const char* const names[8] = {"rawtex0", "rawtex1", "rawtex2", "rawtex3",
                                         "rawtex4", "rawtex5", "rawtex6", "rawtex7"};
    struct rslc_var* raw;
    uv_case(sb, ShaderEnumValue(static_cast<SourceRow>(std::to_underlying(SourceRow::Tex0) + i)));
    rslc_b_begin_block(sb->b);
    raw = uv_load(v, names[i], sb->f[3], v->load_float3_rawtex,
                  uv_rawtex_offset(v, sb_i(sb, static_cast<int32_t>(i / 4)),
                                   sb_i(sb, static_cast<int32_t>(i % 4))),
                  v->rawtex[i]);
    sb_set(sb, sb_ref(sb, coord),
           sb_con(sb, sb->f[4], 4, sb_swz(sb, sb_ref(sb, raw), "x"),
                  sb_swz(sb, sb_ref(sb, raw), "y"), sb_f(sb, 1.0), sb_f(sb, 1.0)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, uv_has(v, VB_HAS_UV0 << i), then_s, 0));
    uv_break(sb);
  }
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, BFE(sourcerow, tex_mtx_info), body));

  // Input form of AB11 sets z element to 1.0
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b,
                        sb_bin(sb, RSLC_B_EQ, BFE(inputform, tex_mtx_info),
                               sb_u(sb, ShaderEnumValue(TexInputForm::AB11))),
                        uv_stmt_set(sb, sb_swz(sb, sb_ref(sb, coord), "z"), sb_f(sb, 1.0)), 0));

  // Convert NaNs to 1 - needed to fix eyelids in Shadow the Hedgehog during cutscenes
  // See https://bugs.dolphin-emu.org/issues/11458
  {
    static const char* const c[3] = {"x", "y", "z"};
    int k;
    for (k = 0; k < 3; k++)
      rslc_b_stmt(sb->b,
                  rslc_b_if(sb->b, sb_isnan(sb, v->isnan_fn, sb_swz(sb, sb_ref(sb, coord), c[k])),
                            uv_stmt_set(sb, sb_swz(sb, sb_ref(sb, coord), c[k]), sb_f(sb, 1.0)),
                            0));
  }

  // first transformation
  texgentype =
      rslc_b_local(sb->b, "texgentype", sb->u[1], BFE(texgentype, tex_mtx_info), false);
  output_tex = rslc_b_local(sb->b, "output_tex", sb->f[3], 0, false);

  rslc_b_begin_block(sb->b);
  uv_case(sb, ShaderEnumValue(TexGenType::EmbossMap));
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var *light, *source, *ldir;
    light = rslc_b_local(sb->b, "light", sb->u[1], BFE(embosslightshift, tex_mtx_info), false);
    source =
        rslc_b_local(sb->b, "source", sb->u[1], BFE(embosssourceshift, tex_mtx_info), false);
    rslc_b_begin_block(sb->b);
    for (i = 0; i < num_texgen; i++)
    {
      uv_case(sb, i);
      sb_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
             uv_o(v, v->layout->tex0 + static_cast<int>(i)));
      uv_break(sb);
    }
    rslc_b_stmt(sb->b, rslc_b_default(sb->b));
    sb_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
           sb_con(sb, sb->f[3], 3, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));
    uv_break(sb);
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, source), body));
    ldir = rslc_b_local(
        sb->b, "ldir", sb->f[3],
        sb_call(sb, "normalize", 1,
                sb_bin(sb, RSLC_B_SUB,
                       sb_swz(sb,
                              sb_mem(sb, sb_idx(sb, uv_u(v, SU_LIGHTS), sb_ref(sb, light)), 3),
                              "xyz"),
                       sb_swz(sb, sb_ref(sb, v->pos), "xyz"))),
        false);
    sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
              sb_con(sb, sb->f[3], 3,
                     sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, v->tangent)),
                     sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, v->binormal)),
                     sb_f(sb, 0.0)));
  }
  rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
  uv_break(sb);
  {
    static const TexGenType types[2] = {TexGenType::Color0, TexGenType::Color1};
    int k;
    for (k = 0; k < 2; k++)
    {
      const int c = k ? v->layout->colors_1 : v->layout->colors_0;
      uv_case(sb, ShaderEnumValue(types[k]));
      sb_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
             sb_con(sb, sb->f[3], 3, sb_swz(sb, uv_o(v, c), "x"), sb_swz(sb, uv_o(v, c), "y"),
                    sb_f(sb, 1.0)));
      uv_break(sb);
    }
  }
  uv_case(sb, ShaderEnumValue(TexGenType::Regular));
  rslc_b_stmt(sb->b, rslc_b_default(sb->b));
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var* tmp;
    rslc_b_begin_block(sb->b);
    if (v->vertex_loader)
    {
      uint32_t a[2];
      a[0] = sb_ref(sb, v->vertex_base_offset);
      a[1] = uv_rawtex_offset(v, sb_bin(sb, RSLC_B_DIV, sb_ref(sb, texgen), sb_i(sb, 4)),
                              sb_bin(sb, RSLC_B_MOD, sb_ref(sb, texgen), sb_i(sb, 4)));
      tmp = rslc_b_local(
          sb->b, "tmp", sb->i[1],
          sb_con(sb, sb->i[1], 1,
                 sb_swz(sb, rslc_b_call(sb->b, v->load_float3_rawtex, a, 2), "z")),
          false);
    }
    else
    {
      // This is messy, due to dynamic indexing of the input texture coordinates.
      // Hopefully the compiler will unroll this whole loop anyway and the switch.
      tmp = rslc_b_local(sb->b, "tmp", sb->i[1], sb_i(sb, 0), false);
      rslc_b_begin_block(sb->b);
      for (i = 0; i < num_texgen; i++)
      {
        uv_case(sb, i);
        sb_set(sb, sb_ref(sb, tmp),
               sb_con(sb, sb->i[1], 1, sb_swz(sb, sb_ref(sb, v->rawtex[i]), "z")));
        uv_break(sb);
      }
      body = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, texgen), body));
    }
    uv_projection(v, tex_mtx_info, output_tex, coord, SU_TRANSFORMMATRICES, uv_tmp_index, tmp);
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    uv_projection(v, tex_mtx_info, output_tex, coord, SU_TEXMATRICES, uv_texgen_index, texgen);
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_NE,
                                 sb_bin(sb, RSLC_B_BAND, uv_u(v, SU_COMPONENTS),
                                        sb_bin(sb, RSLC_B_SHL,
                                               sb_u(sb, std::to_underlying(VB_HAS_TEXMTXIDX0)),
                                               sb_ref(sb, texgen))),
                                 sb_u(sb, 0u)),
                          then_s, else_s));
  }
  rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
  uv_break(sb);
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, texgentype), body));
#undef BFE

  rslc_b_begin_block(sb->b);
  {
    static const char* const p_names[3] = {"P0", "P1", "P2"};
    struct rslc_var *post_mtx_info, *base_index, *p[3];
    uint32_t dots[3];
    int k;
    post_mtx_info = rslc_b_local(
        sb->b, "postMtxInfo", sb->u[1],
        sb_swz(sb, sb_idx(sb, uv_u(v, SU_PACK1), sb_ref(sb, texgen)), "y"), false);
    base_index = rslc_b_local(sb->b, "base_index", sb->u[1],
                              BuildBitfieldExtract<&PostMtxInfo::index>(
                                  sb, &v->bitfield, sb_ref(sb, post_mtx_info)),
                              false);
    for (k = 0; k < 3; k++)
    {
      uint32_t idx = sb_ref(sb, base_index);
      if (k)
        idx = sb_bin(sb, RSLC_B_ADD, idx, sb_u(sb, static_cast<u32>(k)));
      p[k] = rslc_b_local(
          sb->b, p_names[k], sb->f[4],
          sb_idx(sb, uv_u(v, SU_POSTTRANSFORMMATRICES),
                 sb_bin(sb, RSLC_B_BAND, idx, sb_u(sb, 0x3fu))),
          false);
    }
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b,
                  sb_bin(sb, RSLC_B_NE,
                         BuildBitfieldExtract<&PostMtxInfo::normalize>(
                             sb, &v->bitfield, sb_ref(sb, post_mtx_info)),
                         sb_u(sb, 0u)),
                  uv_stmt_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
                              sb_call(sb, "normalize", 1,
                                      sb_swz(sb, sb_ref(sb, output_tex), "xyz"))),
                  0));
    // multiply by postmatrix
    for (k = 0; k < 3; k++)
      dots[k] = sb_bin(sb, RSLC_B_ADD,
                       sb_call(sb, "dot", 2, sb_swz(sb, sb_ref(sb, p[k]), "xyz"),
                               sb_swz(sb, sb_ref(sb, output_tex), "xyz")),
                       sb_swz(sb, sb_ref(sb, p[k]), "w"));
    sb_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xyz"),
           sb_con(sb, sb->f[3], 3, dots[0], dots[1], dots[2]));
  }
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                               sb_bin(sb, RSLC_B_NE, uv_u(v, SU_DUALTEXINFO), sb_u(sb, 0u)),
                               then_s, 0));

  // When q is 0, the GameCube appears to have a special case
  // This can be seen in devkitPro's neheGX Lesson08 example for Wii
  // Makes differences in Rogue Squadron 3 (Hoth sky) and The Last Story (shadow culling)
  rslc_b_stmt(
      sb->b,
      rslc_b_if(sb->b,
                sb_bin(sb, RSLC_B_AND,
                       sb_bin(sb, RSLC_B_EQ, sb_ref(sb, texgentype),
                              sb_u(sb, ShaderEnumValue(TexGenType::Regular))),
                       sb_bin(sb, RSLC_B_EQ, sb_swz(sb, sb_ref(sb, output_tex), "z"),
                              sb_f(sb, 0.0))),
                uv_stmt_set(sb, sb_swz(sb, sb_ref(sb, output_tex), "xy"),
                            sb_call(sb, "clamp", 3,
                                    sb_bin(sb, RSLC_B_DIV, sb_swz(sb, sb_ref(sb, output_tex), "xy"),
                                           sb_f(sb, 2.0)),
                                    sb_con(sb, sb->f[2], 2, sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0)),
                                           sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0))),
                                    sb_con(sb, sb->f[2], 2, sb_f(sb, 1.0), sb_f(sb, 1.0)))),
                0));

  // Hopefully GPUs that can support dynamic indexing will optimize this.
  rslc_b_begin_block(sb->b);
  for (i = 0; i < num_texgen; i++)
  {
    uv_case(sb, i);
    sb_set(sb, uv_o(v, v->layout->tex0 + static_cast<int>(i)), sb_ref(sb, output_tex));
    uv_break(sb);
  }
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, texgen), body));
}

/* GenVertexShaderTexGens */
void uv_texgens(const struct uv_build* v)
{
  const struct shader_build* sb = v->sb;
  struct rslc_var* texgen;
  uint32_t body, init;
  u32 i;
  // The HLSL compiler complains that the output texture coordinates are uninitialized when trying
  // to dynamically index them.
  for (i = 0; i < v->num_texgen; i++)
    sb_set(sb, uv_o(v, v->layout->tex0 + static_cast<int>(i)),
           sb_con(sb, sb->f[3], 3, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));

  // Texture coordinate generation
  if (v->num_texgen == 1)
  {
    rslc_b_begin_block(sb->b);
    texgen = rslc_b_local(sb->b, "texgen", sb->u[1], sb_u(sb, 0u), true);
    uv_texgen_body(v, texgen);
    rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
  }
  else
  {
    init = rslc_b_decl_stmt(sb->b, "texgen", sb->u[1], sb_u(sb, 0u), &texgen);
    rslc_b_begin_block(sb->b);
    uv_texgen_body(v, texgen);
    body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_for(sb->b, init,
                                  sb_bin(sb, RSLC_B_LT, sb_ref(sb, texgen), sb_u(sb, v->num_texgen)),
                                  sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, texgen)), body));
  }
}

/* (gl_VertexID & bit) != 0 */
uint32_t uv_vid_bit(const struct shader_build* sb, int bit)
{
  return sb_bin(sb, RSLC_B_NE, sb_bin(sb, RSLC_B_BAND, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, bit)),
                sb_i(sb, 0));
}

/* if (components & bit) { [load raw] _raw = raw; } else { _raw = cached.xyz; } */
void uv_raw_or_cached(const struct uv_build* v, struct rslc_var* dst, u32 bit, const char* name,
                      int offset, struct rslc_var* attribute, int cached)
{
  const struct shader_build* sb = v->sb;
  struct rslc_var* raw;
  uint32_t then_s, else_s;
  rslc_b_begin_block(sb->b);
  raw = uv_load(v, name, sb->f[3], v->load_float3_float3, uv_u(v, offset), attribute);
  sb_set(sb, sb_ref(sb, dst), sb_ref(sb, raw));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  sb_set(sb, sb_ref(sb, dst), sb_swz(sb, uv_u(v, cached), "xyz"));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, uv_has(v, bit), then_s, else_s));
}

/* float3(dot(N0, x), dot(N1, x), dot(N2, x)) */
uint32_t uv_ndot(const struct shader_build* sb, struct rslc_var* const* n, struct rslc_var* x)
{
  return sb_con(sb, sb->f[3], 3, sb_call(sb, "dot", 2, sb_ref(sb, n[0]), sb_ref(sb, x)),
                sb_call(sb, "dot", 2, sb_ref(sb, n[1]), sb_ref(sb, x)),
                sb_call(sb, "dot", 2, sb_ref(sb, n[2]), sb_ref(sb, x)));
}
}  // namespace

bool BuildVertexShader(struct shader_build* sb, const void* arg)
{
  const VertexShaderBuildArgs* a = static_cast<const VertexShaderBuildArgs*>(arg);
  const ShaderHostConfig& host_config = *a->host_config;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool per_pixel_lighting = host_config.per_pixel_lighting;
  const u32 num_texgen = a->uid_data->num_texgens;
  static const char* const tex_names[8] = {"tex0", "tex1", "tex2", "tex3",
                                           "tex4", "tex5", "tex6", "tex7"};
  struct uv_build v;
  struct vs_output_layout plain;
  struct rslc_b_quals q;
  const struct rslc_type* types[48];
  const char* names[48];
  const struct rslc_type *light, *vs_output;
  struct rslc_var *vs = nullptr, *varying[20];
  struct rslc_var *n[3], *raw_n, *raw_t, *raw_b, *vertex_color[2];
  struct rslc_var *clip_dist0 = nullptr, *clip_dist1 = nullptr;
  struct rslc_func* fmain;
  uint32_t then_s, else_s, e;
  u32 k, i;

  memset(&v, 0, sizeof(v));
  memset(varying, 0, sizeof(varying));
  v.sb = sb;
  v.host_config = &host_config;
  v.api_type = a->api_type;
  v.num_texgen = num_texgen;
  v.vertex_loader =
      host_config.backend_dynamic_vertex_loader || host_config.backend_vs_point_line_expand;
  v.layout = &plain;

  light = BuildLightStruct(sb);

  // uniforms
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 2);
  k = BuildShaderUniforms(sb, light, types, names, 0);
  rslc_b_block_q(sb->b, &q, "VSBlock", nullptr, types, names, k, v.u);

  if (v.vertex_loader)
  {
    rslc_b_quals_init(&q);
    sb_ubo(sb, &q, 4);
    k = BuildGeometryShaderUniforms(sb, types, names, 0);
    rslc_b_block_q(sb->b, &q, "GSBlock", nullptr, types, names, k, v.gs);
  }

  BuildVSOutputLayout(sb, num_texgen, host_config, 0, &plain);
  vs_output = rslc_b_struct(sb->b, "VS_OUTPUT", plain.types, plain.names, plain.n);

  v.isnan_fn = BuildIsNanHeader(sb, a->api_type);
  BuildBitfieldExtractHeader(sb, host_config, &v.bitfield);
  v.calculate_lighting = BuildLightingFunction(sb, v.u[SU_LIGHTS]);

  if (v.vertex_loader)
  {
    types[0] = rslc_b_array(sb->b, sb->u[1], 0);
    names[0] = "vertex_buffer";
    rslc_b_quals_init(&q);
    sb_ssbo(sb, &q, 1);
    q.access = RSLC_B_READONLY;
    rslc_b_block_q(sb->b, &q, "Vertices", nullptr, types, names, 1, &v.vertex_buffer);
    if (a->api_type == APIType::D3D)
    {
      // Offset 0 of vertex_buffer is index 0 with basevertex 0; D3D's SV_VertexID starts at 0
      // regardless of the base vertex, so it is applied here. D3D12 uses a root constant.
      types[0] = sb->u[1];
      names[0] = "base_vertex";
      rslc_b_quals_init(&q);
      sb_ubo(sb, &q, 5);
      rslc_b_block_q(sb->b, &q, "DX_Constants", nullptr, types, names, 1, &v.base_vertex);
    }
    uv_loader_functions(&v);
  }
  else
  {
    static const char* const rawtex_names[8] = {"rawtex0", "rawtex1", "rawtex2", "rawtex3",
                                                "rawtex4", "rawtex5", "rawtex6", "rawtex7"};
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
#define UV_ATTR(var, attr, name, type)                                                             \
  do                                                                                               \
  {                                                                                                \
    q.location = sb_attribute_location(sb, ShaderEnumValue(attr));                                \
    var = rslc_b_declare(sb->b, &q, name, type, 0);                                               \
  } while (0)
    UV_ATTR(v.rawpos, ShaderAttrib::Position, "rawpos", sb->f[4]);
    UV_ATTR(v.posmtx, ShaderAttrib::PositionMatrix, "posmtx", sb->u[4]);
    UV_ATTR(v.rawnormal, ShaderAttrib::Normal, "rawnormal", sb->f[3]);
    UV_ATTR(v.rawtangent, ShaderAttrib::Tangent, "rawtangent", sb->f[3]);
    UV_ATTR(v.rawbinormal, ShaderAttrib::Binormal, "rawbinormal", sb->f[3]);
    UV_ATTR(v.rawcolor[0], ShaderAttrib::Color0, "rawcolor0", sb->f[4]);
    UV_ATTR(v.rawcolor[1], ShaderAttrib::Color1, "rawcolor1", sb->f[4]);
    for (i = 0; i < 8; ++i)
      UV_ATTR(v.rawtex[i], ShaderAttrib::TexCoord0 + i, rawtex_names[i], sb->f[3]);
#undef UV_ATTR
  }

  if (host_config.backend_geometry_shaders)
  {
    struct vs_output_layout out_l;
    BuildVSOutputLayout(sb, num_texgen, host_config, BuildInterpolationQuals(msaa, ssaa), &out_l);
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_OUT;
    q.location = sb_varying_location(sb, 0);
    vs = rslc_b_io_block(sb->b, &q, "VertexData", "vs", out_l.types, out_l.names, nullptr,
                         out_l.quals, out_l.n, -1);
  }
  else
  {
    // Let's set up attributes
    u32 counter = 0;
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_OUT;
    q.quals = BuildInterpolationQuals(msaa, ssaa);
#define UV_VARYING(idx, name, type)                                                                \
  do                                                                                               \
  {                                                                                                \
    q.location = sb_varying_location(sb, counter++);                                              \
    varying[idx] = rslc_b_declare(sb->b, &q, name, type, 0);                                      \
  } while (0)
    UV_VARYING(plain.colors_0, "colors_0", sb->f[4]);
    UV_VARYING(plain.colors_1, "colors_1", sb->f[4]);
    for (i = 0; i < num_texgen; ++i)
      UV_VARYING(plain.tex0 + static_cast<int>(i), tex_names[i], sb->f[3]);
    if (!host_config.fast_depth_calc)
      UV_VARYING(plain.clipPos, "clipPos", sb->f[4]);
    if (per_pixel_lighting)
    {
      UV_VARYING(plain.Normal, "Normal", sb->f[3]);
      UV_VARYING(plain.WorldPos, "WorldPos", sb->f[3]);
    }
#undef UV_VARYING
  }

  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);
  v.o = rslc_b_local(sb->b, "o", vs_output, 0, false);

#define O(k) uv_o(&v, k)
#define OP(c) sb_swz(sb, O(plain.pos), c)
  if (host_config.backend_vs_point_line_expand)
  {
    struct rslc_var* vertex_id =
        rslc_b_local(sb->b, "vertex_id", sb->u[1], sb_bi(sb, "gl_VertexIndex"), false);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, vertex_id),
           sb_bin(sb, RSLC_B_SHR, sb_ref(sb, vertex_id), sb_i(sb, 2)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, sb_ref(sb, v.gs[3]), sb_u(sb, 0u)),
                          then_s, 0));
    e = sb_ref(sb, vertex_id);
    v.vertex_base_offset = rslc_b_local(sb->b, "vertex_base_offset", sb->u[1],
                                        rslc_b_call(sb->b, v.base_offset, &e, 1), false);
  }
  else if (host_config.backend_dynamic_vertex_loader)
  {
    e = sb_bi(sb, "gl_VertexIndex");
    v.vertex_base_offset = rslc_b_local(sb->b, "vertex_base_offset", sb->u[1],
                                        rslc_b_call(sb->b, v.base_offset, &e, 1), false);
  }
  // rawpos is always needed
  v.rawpos = uv_load(&v, "rawpos", sb->f[4], v.load_float4_rawpos, uv_u(&v, SU_OFFSET_RAWPOS),
                     v.rawpos);
  // Transforms
  {
    static const char* const p_names[3] = {"P0", "P1", "P2"};
    static const char* const n_names[3] = {"N0", "N1", "N2"};
    struct rslc_var *posmtx, *posidx, *normidx;
    for (k = 0; k < 3; k++)
      v.p[k] = rslc_b_local(sb->b, p_names[k], sb->f[4], 0, false);
    for (k = 0; k < 3; k++)
      n[k] = rslc_b_local(sb->b, n_names[k], sb->f[3], 0, false);

    rslc_b_begin_block(sb->b);
    posmtx = uv_load(&v, "posmtx", sb->u[4], v.load_uint4_ubyte4, uv_u(&v, SU_OFFSET_POSMTX),
                     v.posmtx);
    // Vertex format has a per-vertex matrix
    posidx = rslc_b_local(sb->b, "posidx", sb->i[1],
                          sb_con(sb, sb->i[1], 1, sb_swz(sb, sb_ref(sb, posmtx), "r")), false);
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_ref(sb, v.p[k]),
             sb_idx(sb, uv_u(&v, SU_TRANSFORMMATRICES),
                    k ? sb_bin(sb, RSLC_B_ADD, sb_ref(sb, posidx), sb_i(sb, static_cast<int>(k))) :
                        sb_ref(sb, posidx)));
    normidx = rslc_b_local(
        sb->b, "normidx", sb->i[1],
        rslc_b_select(sb->b, sb_bin(sb, RSLC_B_GE, sb_ref(sb, posidx), sb_i(sb, 32)),
                      sb_bin(sb, RSLC_B_SUB, sb_ref(sb, posidx), sb_i(sb, 32)),
                      sb_ref(sb, posidx)),
        false);
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_ref(sb, n[k]),
             sb_swz(sb,
                    sb_idx(sb, uv_u(&v, SU_NORMALMATRICES),
                           k ? sb_bin(sb, RSLC_B_ADD, sb_ref(sb, normidx),
                                      sb_i(sb, static_cast<int>(k))) :
                               sb_ref(sb, normidx)),
                    "xyz"));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    // One shared matrix
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_ref(sb, v.p[k]),
             sb_idx(sb, uv_u(&v, SU_POSNORMALMATRIX), sb_i(sb, static_cast<int>(k))));
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_ref(sb, n[k]),
             sb_swz(sb, sb_idx(sb, uv_u(&v, SU_POSNORMALMATRIX), sb_i(sb, 3 + static_cast<int>(k))),
                    "xyz"));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, uv_has(&v, VB_HAS_POSMTXIDX), then_s, else_s));
  }

  // Multiply the position vector by the position matrix
  v.pos = rslc_b_local(
      sb->b, "pos", sb->f[4],
      sb_con(sb, sb->f[4], 4, sb_call(sb, "dot", 2, sb_ref(sb, v.p[0]), sb_ref(sb, v.rawpos)),
             sb_call(sb, "dot", 2, sb_ref(sb, v.p[1]), sb_ref(sb, v.rawpos)),
             sb_call(sb, "dot", 2, sb_ref(sb, v.p[2]), sb_ref(sb, v.rawpos)), sb_f(sb, 1.0)),
      false);
  {
    uint32_t d[4];
    for (k = 0; k < 4; k++)
      d[k] = sb_call(sb, "dot", 2, sb_idx(sb, uv_u(&v, SU_PROJECTION), sb_i(sb, static_cast<int>(k))),
                     sb_ref(sb, v.pos));
    sb_set(sb, O(plain.pos), sb_con(sb, sb->f[4], 4, d[0], d[1], d[2], d[3]));
  }

  raw_n = rslc_b_local(sb->b, "_rawnormal", sb->f[3], 0, false);
  raw_t = rslc_b_local(sb->b, "_rawtangent", sb->f[3], 0, false);
  raw_b = rslc_b_local(sb->b, "_rawbinormal", sb->f[3], 0, false);
  uv_raw_or_cached(&v, raw_n, VB_HAS_NORMAL, "rawnormal", SU_OFFSET_RAWNORMAL, v.rawnormal,
                   SU_CACHED_NORMAL);
  uv_raw_or_cached(&v, raw_t, VB_HAS_TANGENT, "rawtangent", SU_OFFSET_RAWTANGENT, v.rawtangent,
                   SU_CACHED_TANGENT);
  uv_raw_or_cached(&v, raw_b, VB_HAS_BINORMAL, "rawbinormal", SU_OFFSET_RAWBINORMAL,
                   v.rawbinormal, SU_CACHED_BINORMAL);

  // The scale of the transform matrix is used to control the size of the emboss map
  // effect by changing the scale of the transformed binormals (which only get used by
  // emboss map texgens). By normalising the first transformed normal (which is used
  // by lighting calculations and needs to be unit length), the same transform matrix
  // can do double duty, scaling for emboss mapping, and not scaling for lighting.
  v.normal = rslc_b_local(sb->b, "_normal", sb->f[3],
                          sb_call(sb, "normalize", 1, uv_ndot(sb, n, raw_n)), false);
  v.tangent = rslc_b_local(sb->b, "_tangent", sb->f[3], uv_ndot(sb, n, raw_t), false);
  v.binormal = rslc_b_local(sb->b, "_binormal", sb->f[3], uv_ndot(sb, n, raw_b), false);

  // xfmem.numColorChans controls the number of color channels available to TEV,
  // but we still need to generate all channels here, as it can be used in texgen.
  // Cel-damage is an example of this.
  vertex_color[0] = rslc_b_local(sb->b, "vertex_color_0", sb->f[4], 0, false);
  vertex_color[1] = rslc_b_local(sb->b, "vertex_color_1", sb->f[4], 0, false);
  {
    const u32 both = VB_HAS_COL0 | VB_HAS_COL1;
    struct rslc_var *c0, *c1;
    uint32_t col0_s, col1_s, none_s;
    // To use color 1, the vertex descriptor must have color 0 and 1.
    // If color 1 is present but not color 0, it is used for lighting channel 0.
    rslc_b_local(sb->b, "use_color_1", sb->bl,
                 sb_bin(sb, RSLC_B_EQ, sb_bin(sb, RSLC_B_BAND, uv_u(&v, SU_COMPONENTS),
                                              sb_u(sb, both)),
                        sb_u(sb, both)),
                 false);
#define UV_COLOR(k)                                                                                \
  uv_load(&v, (k) ? "rawcolor1" : "rawcolor0", sb->f[4], v.load_float4_ubyte4,                     \
          uv_u(&v, (k) ? SU_OFFSET_RAWCOLOR1 : SU_OFFSET_RAWCOLOR0), v.rawcolor[k])
    rslc_b_begin_block(sb->b);
    c0 = UV_COLOR(0);
    c1 = UV_COLOR(1);
    sb_set(sb, sb_ref(sb, vertex_color[0]), sb_ref(sb, c0));
    sb_set(sb, sb_ref(sb, vertex_color[1]), sb_ref(sb, c1));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    c0 = UV_COLOR(0);
    sb_set(sb, sb_ref(sb, vertex_color[0]), sb_ref(sb, c0));
    sb_set(sb, sb_ref(sb, vertex_color[1]), sb_ref(sb, c0));
    col0_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    c1 = UV_COLOR(1);
    sb_set(sb, sb_ref(sb, vertex_color[0]), sb_ref(sb, c1));
    sb_set(sb, sb_ref(sb, vertex_color[1]), sb_ref(sb, c1));
    col1_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, vertex_color[0]), uv_u(&v, SU_MISSING_COLOR_VALUE));
    sb_set(sb, sb_ref(sb, vertex_color[1]), uv_u(&v, SU_MISSING_COLOR_VALUE));
    none_s = rslc_b_end_block(sb->b);
#undef UV_COLOR
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b,
                  sb_bin(sb, RSLC_B_EQ,
                         sb_bin(sb, RSLC_B_BAND, uv_u(&v, SU_COMPONENTS), sb_u(sb, both)),
                         sb_u(sb, both)),
                  then_s,
                  rslc_b_if(sb->b, uv_has(&v, VB_HAS_COL0), col0_s,
                            rslc_b_if(sb->b, uv_has(&v, VB_HAS_COL1), col1_s, none_s))));
  }

  {
    struct VertexLightingBuild l;
    memset(&l, 0, sizeof(l));
    l.uniforms = v.u;
    l.bitfield = &v.bitfield;
    l.calculate_lighting = v.calculate_lighting;
    l.world_pos.var = v.pos;
    l.world_pos.member = -1;
    l.world_pos.swizzle = "xyz";
    l.normal.var = v.normal;
    l.normal.member = -1;
    for (k = 0; k < 2; k++)
    {
      l.in_color[k].var = vertex_color[k];
      l.in_color[k].member = -1;
      l.out_color[k].var = v.o;
      l.out_color[k].member = k ? plain.colors_1 : plain.colors_0;
    }
    BuildVertexLighting(sb, &l);
  }

  // Texture Coordinates
  if (num_texgen > 0)
    uv_texgens(&v);

  if (host_config.backend_vs_point_line_expand)
  {
    struct vs_expansion x;
    uint32_t line_s, point_s;
    memset(&x, 0, sizeof(x));
    x.o = v.o;
    x.layout = &plain;
    x.projection = v.u[SU_PROJECTION];
    x.clinept = v.gs[1];
    x.texoffset = v.gs[2];

    rslc_b_begin_block(sb->b);
    {
      static const char* const op_names[3] = {"other_p0", "other_p1", "other_p2"};
      struct rslc_var *other_base_offset, *other_rawpos, *op[3];
      uint32_t a2[2];
      x.is_bottom = rslc_b_local(sb->b, "is_bottom", sb->bl, uv_vid_bit(sb, 2), false);
      x.is_right = rslc_b_local(sb->b, "is_right", sb->bl, uv_vid_bit(sb, 1), false);
      other_base_offset = rslc_b_local(sb->b, "other_base_offset", sb->u[1],
                                       sb_ref(sb, v.vertex_base_offset), false);
      rslc_b_begin_block(sb->b);
      sb_op_set(sb, RSLC_B_SUB, sb_ref(sb, other_base_offset), uv_u(&v, SU_VERTEX_STRIDE));
      then_s = rslc_b_end_block(sb->b);
      rslc_b_begin_block(sb->b);
      sb_op_set(sb, RSLC_B_ADD, sb_ref(sb, other_base_offset), uv_u(&v, SU_VERTEX_STRIDE));
      else_s = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_ref(sb, x.is_bottom), then_s, else_s));
      a2[0] = sb_ref(sb, other_base_offset);
      a2[1] = uv_u(&v, SU_OFFSET_RAWPOS);
      other_rawpos = rslc_b_local(sb->b, "other_rawpos", sb->f[4],
                                  rslc_b_call(sb->b, v.load_float4_rawpos, a2, 2), false);
      for (k = 0; k < 3; k++)
        op[k] = rslc_b_local(sb->b, op_names[k], sb->f[4], sb_ref(sb, v.p[k]), false);
      rslc_b_begin_block(sb->b);
      {
        struct rslc_var* other_posidx;
        a2[0] = sb_ref(sb, other_base_offset);
        a2[1] = uv_u(&v, SU_OFFSET_POSMTX);
        other_posidx = rslc_b_local(
            sb->b, "other_posidx", sb->u[1],
            sb_swz(sb, rslc_b_call(sb->b, v.load_uint4_ubyte4, a2, 2), "r"), false);
        for (k = 0; k < 3; k++)
          sb_set(sb, sb_ref(sb, op[k]),
                 sb_idx(sb, uv_u(&v, SU_TRANSFORMMATRICES),
                        k ? sb_bin(sb, RSLC_B_ADD, sb_ref(sb, other_posidx),
                                   sb_i(sb, static_cast<int>(k))) :
                            sb_ref(sb, other_posidx)));
      }
      then_s = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_if(sb->b, uv_has(&v, VB_HAS_POSMTXIDX), then_s, 0));
      x.other_pos = rslc_b_local(
          sb->b, "other_pos", sb->f[4],
          sb_con(sb, sb->f[4], 4,
                 sb_call(sb, "dot", 2, sb_ref(sb, op[0]), sb_ref(sb, other_rawpos)),
                 sb_call(sb, "dot", 2, sb_ref(sb, op[1]), sb_ref(sb, other_rawpos)),
                 sb_call(sb, "dot", 2, sb_ref(sb, op[2]), sb_ref(sb, other_rawpos)),
                 sb_f(sb, 1.0)),
          false);
      BuildVSLineExpansion(sb, &x);
    }
    line_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    x.is_bottom = rslc_b_local(sb->b, "is_bottom", sb->bl, uv_vid_bit(sb, 2), false);
    x.is_right = rslc_b_local(sb->b, "is_right", sb->bl, uv_vid_bit(sb, 1), false);
    BuildVSPointExpansion(sb, &x);
    point_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(
        sb->b,
        rslc_b_if(sb->b,
                  sb_bin(sb, RSLC_B_EQ, sb_ref(sb, v.gs[3]),
                         sb_u(sb, std::to_underlying(VSExpand::Line))),
                  line_s,
                  rslc_b_if(sb->b,
                            sb_bin(sb, RSLC_B_EQ, sb_ref(sb, v.gs[3]),
                                   sb_u(sb, std::to_underlying(VSExpand::Point))),
                            point_s, 0)));
  }

  if (per_pixel_lighting)
  {
    // When per-pixel lighting is enabled, the vertex colors are passed through
    // unmodified so we can evaluate the lighting in the pixel shader.
    // Lighting is also still computed in the vertex shader since it can be used to
    // generate texture coordinates. We generated them above, so now the colors can
    // be reverted to their previous stage.
    sb_set(sb, O(plain.colors_0), sb_ref(sb, vertex_color[0]));
    sb_set(sb, O(plain.colors_1), sb_ref(sb, vertex_color[1]));
    // Note that the numColorChans logic should be (but currently isn't)
    // performed in the pixel shader.
  }
  else
  {
    // The number of colors available to TEV is determined by numColorChans.
    // We have to provide the fields to match the interface, so set to zero
    // if it's not enabled.
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_EQ, uv_u(&v, SU_NUMCOLORCHANS), sb_u(sb, 0u)),
                          uv_stmt_set(sb, O(plain.colors_0),
                                      sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0),
                                             sb_f(sb, 0.0), sb_f(sb, 0.0))),
                          0));
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_LE, uv_u(&v, SU_NUMCOLORCHANS), sb_u(sb, 1u)),
                          uv_stmt_set(sb, O(plain.colors_1),
                                      sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0),
                                             sb_f(sb, 0.0), sb_f(sb, 0.0))),
                          0));
  }

  // clipPos/w needs to be done in pixel shader, not here
  if (!host_config.fast_depth_calc)
    sb_set(sb, O(plain.clipPos), O(plain.pos));

  if (per_pixel_lighting)
  {
    sb_set(sb, O(plain.Normal), sb_ref(sb, v.normal));
    sb_set(sb, O(plain.WorldPos), sb_swz(sb, sb_ref(sb, v.pos), "xyz"));
  }

#define CC(c) sb_swz(sb, uv_u(&v, SU_PIXELCENTERCORRECTION), c)
  // If we can disable the incorrect depth clipping planes using depth clamping, then we can do
  // our own depth clipping and calculate the depth range before the perspective divide if
  // necessary.
  if (host_config.backend_depth_clamp)
  {
    // Since we're adjusting z for the depth range before the perspective divide, we have to do our
    // own clipping. We want to clip so that -w <= z <= 0, which matches the console -1..0 range.
    // We adjust our depth value for clipping purposes to match the perspective projection in the
    // software backend, which is a hack to fix Sonic Adventure and Unleashed games.
    struct rslc_var* clip_depth = rslc_b_local(
        sb->b, "clipDepth", sb->f[1],
        sb_bin(sb, RSLC_B_MUL, OP("z"), sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), sb_f(sb, 1e-7))),
        false);
    clip_dist0 = rslc_b_local(sb->b, "clipDist0", sb->f[1],
                              sb_bin(sb, RSLC_B_ADD, sb_ref(sb, clip_depth), OP("w")), false);
    clip_dist1 = rslc_b_local(sb->b, "clipDist1", sb->f[1],
                              sb_un(sb, RSLC_B_NEG, sb_ref(sb, clip_depth)), false);  // Far: z > 0
    if (host_config.backend_geometry_shaders)
    {
      sb_set(sb, O(plain.clipDist0), sb_ref(sb, clip_dist0));
      sb_set(sb, O(plain.clipDist1), sb_ref(sb, clip_dist1));
    }
  }

  // Write the true depth value. If the game uses depth textures, then the pixel shader will
  // override it with the correct values if not then early z culling will improve speed.
  // There are two different ways to do this, when the depth range is oversized, we process
  // the depth range in the vertex shader, if not we let the host driver handle it.
  //
  // Adjust z for the depth range. We're using an equation which incorporates a depth inversion,
  // so we can map the console -1..0 range to the 0..1 range used in the depth buffer.
  // We have to handle the depth range in the vertex shader instead of after the perspective
  // divide, because some games will use a depth range larger than what is allowed by the
  // graphics API. These large depth ranges will still be clipped to the 0..1 range, so these
  // games effectively add a depth bias to the values written to the depth buffer.
  sb_set(sb, OP("z"),
         sb_bin(sb, RSLC_B_SUB, sb_bin(sb, RSLC_B_MUL, OP("w"), CC("w")),
                sb_bin(sb, RSLC_B_MUL, OP("z"), CC("z"))));

  if (!host_config.backend_clip_control)
  {
    // If the graphics API doesn't support a depth range of 0..1, then we need to map z to
    // the -1..1 range. Unfortunately we have to use a subtraction, which is a lossy floating-point
    // operation that can introduce a round-trip error.
    sb_set(sb, OP("z"),
           sb_bin(sb, RSLC_B_SUB, sb_bin(sb, RSLC_B_MUL, OP("z"), sb_f(sb, 2.0)), OP("w")));
  }

  // Correct for negative viewports by mirroring all vertices. We need to negate the height here,
  // since the viewport height is already negated by the render backend.
  sb_op_set(sb, RSLC_B_MUL, OP("xy"),
            sb_call(sb, "sign", 1,
                    sb_bin(sb, RSLC_B_MUL, CC("xy"),
                           sb_con(sb, sb->f[2], 2, sb_f(sb, 1.0),
                                  sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0))))));

  // The console GPU places the pixel center at 7/12 in screen space unless
  // antialiasing is enabled, while D3D and OpenGL place it at 0.5. This results
  // in some primitives being placed one pixel too far to the bottom-right,
  // which in turn can be critical if it happens for clear quads.
  // Hence, we compensate for this pixel center difference so that primitives
  // get rasterized correctly.
  sb_set(sb, OP("xy"), sb_bin(sb, RSLC_B_SUB, OP("xy"), sb_bin(sb, RSLC_B_MUL, OP("w"), CC("xy"))));
#undef CC

  if (host_config.vertex_rounding)
  {
    // By now our position is in clip space. However, higher resolutions than the Wii outputs
    // cause an additional pixel offset. Due to a higher pixel density we need to correct this
    // by converting our clip-space position into the Wii's screen-space.
    // Acquire the right pixel and then convert it back.
#define HALF_VP(c)                                                                                 \
  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, uv_u(&v, SU_VIEWPORT_SIZE), c), sb_f(sb, 0.5))
    struct rslc_var *ss_x, *ss_y;
    rslc_b_begin_block(sb->b);
    ss_x = rslc_b_local(sb->b, "ss_pixel_x", sb->f[1],
                        sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_ADD, OP("x"), sb_f(sb, 1.0)),
                               HALF_VP("x")),
                        false);
    ss_y = rslc_b_local(sb->b, "ss_pixel_y", sb->f[1],
                        sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_ADD, OP("y"), sb_f(sb, 1.0)),
                               HALF_VP("y")),
                        false);
    sb_set(sb, sb_ref(sb, ss_x), sb_call(sb, "round", 1, sb_ref(sb, ss_x)));
    sb_set(sb, sb_ref(sb, ss_y), sb_call(sb, "round", 1, sb_ref(sb, ss_y)));
    sb_set(sb, OP("x"),
           sb_bin(sb, RSLC_B_SUB, sb_bin(sb, RSLC_B_DIV, sb_ref(sb, ss_x), HALF_VP("x")),
                  sb_f(sb, 1.0)));
    sb_set(sb, OP("y"),
           sb_bin(sb, RSLC_B_SUB, sb_bin(sb, RSLC_B_DIV, sb_ref(sb, ss_y), HALF_VP("y")),
                  sb_f(sb, 1.0)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, OP("w"), sb_f(sb, 1.0)), then_s, 0));
#undef HALF_VP
  }

  if (host_config.backend_geometry_shaders)
  {
    struct sb_place vs_place = {vs, nullptr, -1}, o_place = {v.o, nullptr, -1};
    BuildAssignVSOutputMembers(sb, &plain, &vs_place, &o_place, host_config);
  }
  else
  {
    // TODO: Pass interface blocks between shader stages even if geometry shaders
    // are not supported, however that will require at least OpenGL 3.2 support.
    for (i = 0; i < num_texgen; ++i)
    {
      const int t = plain.tex0 + static_cast<int>(i);
      sb_set(sb, sb_swz(sb, sb_ref(sb, varying[t]), "xyz"), O(t));
    }
    if (!host_config.fast_depth_calc)
      sb_set(sb, sb_ref(sb, varying[plain.clipPos]), O(plain.clipPos));
    if (per_pixel_lighting)
    {
      sb_set(sb, sb_ref(sb, varying[plain.Normal]), O(plain.Normal));
      sb_set(sb, sb_ref(sb, varying[plain.WorldPos]), O(plain.WorldPos));
    }
    sb_set(sb, sb_ref(sb, varying[plain.colors_0]), O(plain.colors_0));
    sb_set(sb, sb_ref(sb, varying[plain.colors_1]), O(plain.colors_1));
  }

  if (host_config.backend_depth_clamp)
  {
    sb_set(sb, sb_idx(sb, sb_bi(sb, "gl_ClipDistance"), sb_i(sb, 0)), sb_ref(sb, clip_dist0));
    sb_set(sb, sb_idx(sb, sb_bi(sb, "gl_ClipDistance"), sb_i(sb, 1)), sb_ref(sb, clip_dist1));
  }

  // Vulkan NDC space has Y pointing down (right-handed NDC space).
  if (a->api_type == APIType::Vulkan)
    sb_set(sb, sb_bi(sb, "gl_Position"),
           sb_con(sb, sb->f[4], 4, OP("x"), sb_un(sb, RSLC_B_NEG, OP("y")), OP("z"), OP("w")));
  else
    sb_set(sb, sb_bi(sb, "gl_Position"), O(plain.pos));
#undef OP
#undef O
  return rslc_b_func_end(sb->b);
}
}  // namespace UberShader
