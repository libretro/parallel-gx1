// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderGen.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

VertexShaderUid GetVertexShaderUid()
{
  ASSERT(bpmem.genMode.numtexgens == xfmem.numTexGen.numTexGens);
  ASSERT(bpmem.genMode.numcolchans == xfmem.numChan.numColorChans);

  VertexShaderUid out;
  vertex_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;
  uid_data->numColorChans = xfmem.numChan.numColorChans;

  uid_data->components =
      VertexLoaderManager::g_current_components & (VB_HAS_SHARED | VB_HAS_TEXMTXIDXALL);

  // Move UV components into texcoord_elem_count
  for (u32 i = 0; i < 8; i++)
  {
    if (VertexLoaderManager::g_current_components & (VB_HAS_UV0 << i))
    {
      // Hardcode to 2 components (ApplyDriverBugs will replace this with an exact count if needed)
      uid_data->texGenInfo[i].texcoord_elem_count = 2;
    }
  }

  GetLightingShaderUid(uid_data->lighting);

  // transform texcoords
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];

    // sourcerow, inputform and texgentype each have an extra bit that gets ignored later.
    // Validate and eliminate them now to save space in the UID.
    ASSERT(xfmem.texMtxInfo[i].sourcerow <= SourceRow::Tex7);
    texinfo.sourcerow = xfmem.texMtxInfo[i].sourcerow;

    texinfo.inputform = xfmem.texMtxInfo[i].inputform == TexInputForm::ABC1 ? TexInputForm::ABC1 :
                                                                              TexInputForm::AB11;
    auto texgentype = xfmem.texMtxInfo[i].texgentype;
    // first transformation
    switch (texgentype)
    {
    default:
    case TexGenType::Regular:
      texinfo.is_regular_texgen = true;
      texinfo.regular.projection = xfmem.texMtxInfo[i].projection;

      // only put dualTexTrans_enabled in UID if we have at least one regular texgen.
      uid_data->dualTexTrans_enabled = xfmem.dualTexTrans.enabled;

      // CHECKME: does this only work for regular tex gen types?
      if (uid_data->dualTexTrans_enabled)
      {
        texinfo.regular.postmtx_index = xfmem.postMtxInfo[i].index;
        texinfo.regular.postmtx_normalize = xfmem.postMtxInfo[i].normalize;
      }
      break;
    case TexGenType::EmbossMap:  // calculate tex coords into bump map
      texinfo.is_regular_texgen = false;
      texinfo.other.texgentype = texgentype;

      // transform the light dir into tangent space
      texinfo.other.emboss_sourceshift = xfmem.texMtxInfo[i].embosssourceshift;
      if ((uid_data->components & (VB_HAS_TANGENT | VB_HAS_BINORMAL)) != 0)
        texinfo.other.emboss_lightshift = xfmem.texMtxInfo[i].embosslightshift;
      break;
    case TexGenType::Color0:
    case TexGenType::Color1:
      texinfo.is_regular_texgen = false;
      texinfo.other.texgentype = texgentype;
      break;
    }
  }

  return out;
}

/* ---- the stage built by calls ---- */

namespace
{
/* DolphinVertexInput's and DolphinVertexOutput's members */
enum
{
  VIN_COLOR_0,
  VIN_COLOR_1,
  VIN_POSITION,
  VIN_NORMAL,
  VIN_BINORMAL,
  VIN_TANGENT,
  VIN_TEXTURE_COORD_0
};
enum
{
  VOUT_COLOR_0,
  VOUT_COLOR_1,
  VOUT_POSITION,
  VOUT_NORMAL,
  VOUT_TEXTURE_COORD_0
};
/* the raw normal, tangent and binormal, as VB_HAS_NORMAL << k orders them */
enum
{
  RAW_NORMAL,
  RAW_TANGENT,
  RAW_BINORMAL
};

struct vs_build
{
  const struct shader_build* sb;
  const vertex_shader_uid_data* uid_data;
  const ShaderHostConfig* host_config;
  APIType api_type;
  bool expand;
  struct rslc_var* u[SU_COUNT];
  struct rslc_var* gs[4];
  struct rslc_var* base_vertex;
  struct rslc_var* input_buffer;
  const struct rslc_type* input_data;
  int in_posmtx, in_pos, in_raw[3], in_color[2], in_tex[8];
  struct rslc_var *rawpos, *posmtx, *raw[3], *rawcolor[2], *rawtex[8];
  struct rslc_func *isnan_fn, *lighting[NUM_XF_COLOR_CHANNELS], *unpack, *expand_data;
  struct rslc_func *position_matrix, *normal_matrix, *transform_texcoord[8];
};

uint32_t vb_u(const struct vs_build* v, int k)
{
  return sb_ref(v->sb, v->u[k]);
}

/* uniform[k] */
uint32_t vb_ui(const struct vs_build* v, int k, uint32_t index)
{
  return sb_idx(v->sb, vb_u(v, k), index);
}

uint32_t vb_mem(const struct vs_build* v, struct rslc_var* var, int k)
{
  return sb_mem(v->sb, sb_ref(v->sb, var), static_cast<uint32_t>(k));
}

/* f(args); as a statement */
void vb_call_stmt(const struct vs_build* v, const struct rslc_func* f, const uint32_t* args,
                  uint32_t n)
{
  sb_do(v->sb, rslc_b_call(v->sb->b, f, args, n));
}

/* if (cond) l = r; */
void vb_if_set(const struct vs_build* v, uint32_t cond, uint32_t l, uint32_t r)
{
  struct rslc_builder* b = v->sb->b;
  rslc_b_stmt(b, rslc_b_if(b, cond, rslc_b_expr(b, rslc_b_assign(b, RSLC_B_SET, l, r)), 0));
}

/* -x, of a float literal as the text writes it */
uint32_t vb_negf(const struct shader_build* sb, double x)
{
  return sb_un(sb, RSLC_B_NEG, sb_f(sb, x));
}

/* InputData dolphin_primitive_expand_data(int index_offset) */
void vb_expand_data(struct vs_build* v)
{
  static const char* const names[1] = {"index_offset"};
  static const unsigned dirs[1] = {RSLC_B_IN};
  const struct shader_build* sb = v->sb;
  const struct rslc_type* types[1];
  struct rslc_var* vertex_id;
  uint32_t e;
  types[0] = sb->i[1];
  v->expand_data =
      rslc_b_func(sb->b, "dolphin_primitive_expand_data", v->input_data, 1, types, dirs, names);
  rslc_b_func_begin(sb->b, v->expand_data);
  if (v->api_type == APIType::D3D)
  {
    // D3D doesn't include the base vertex in SV_VertexID
    // See comment in UberShaderVertex for details
    e = sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_SHR, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, 2)),
               sb_ref(sb, v->base_vertex));
  }
  else
  {
    e = sb_bin(sb, RSLC_B_SHR, sb_con(sb, sb->u[1], 1, sb_bi(sb, "gl_VertexIndex")),
               sb_u(sb, 2u));
  }
  vertex_id = rslc_b_local(sb->b, "vertex_id", sb->u[1], e, false);
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b, sb_idx(sb, sb_ref(sb, v->input_buffer),
                                          sb_bin(sb, RSLC_B_ADD, sb_ref(sb, vertex_id),
                                                 sb_ref(sb, rslc_b_param(v->expand_data, 0))))));
  rslc_b_func_end(sb->b);
}

/* int posidx = int(posmtx.r); with the expanding input's posmtx unpacked first */
struct rslc_var* vb_posidx(const struct vs_build* v)
{
  const struct shader_build* sb = v->sb;
  struct rslc_var* posmtx = v->posmtx;
  if (v->expand)
  {
    uint32_t zero = sb_i(sb, 0);
    struct rslc_var* i =
        rslc_b_local(sb->b, "i", v->input_data, rslc_b_call(sb->b, v->expand_data, &zero, 1),
                     false);
    uint32_t packed = vb_mem(v, i, v->in_posmtx);
    posmtx = rslc_b_local(sb->b, "posmtx", sb->u[4], rslc_b_call(sb->b, v->unpack, &packed, 1),
                          false);
  }
  return rslc_b_local(sb->b, "posidx", sb->i[1],
                      sb_con(sb, sb->i[1], 1, sb_swz(sb, sb_ref(sb, posmtx), "r")), false);
}

/* idx + k, or idx alone for 0 */
uint32_t vb_plus(const struct shader_build* sb, struct rslc_var* idx, int k)
{
  if (!k)
    return sb_ref(sb, idx);
  return sb_bin(sb, RSLC_B_ADD, sb_ref(sb, idx), sb_i(sb, k));
}

void vb_transform_matrices(struct vs_build* v)
{
  const struct shader_build* sb = v->sb;
  const bool has_posmtx = (v->uid_data->components & VB_HAS_POSMTXIDX) != 0;
  struct rslc_var* result;
  int k;

  v->position_matrix = rslc_b_func(sb->b, "dolphin_position_matrix", rslc_b_mat(sb->b, 3, 4), 0,
                                   nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, v->position_matrix);
  result = rslc_b_local(sb->b, "result", rslc_b_mat(sb->b, 3, 4), 0, false);
  if (has_posmtx)
  {
    // Vertex format has a per-vertex matrix
    struct rslc_var* posidx = vb_posidx(v);
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_idx(sb, sb_ref(sb, result), sb_i(sb, k)),
             vb_ui(v, SU_TRANSFORMMATRICES, vb_plus(sb, posidx, k)));
  }
  else
  {
    // One shared matrix
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_idx(sb, sb_ref(sb, result), sb_i(sb, k)),
             vb_ui(v, SU_POSNORMALMATRIX, sb_i(sb, k)));
  }
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_ref(sb, result)));
  rslc_b_func_end(sb->b);

  // The scale of the transform matrix is used to control the size of the emboss map effect, by
  // changing the scale of the transformed binormals (which only get used by emboss map texgens).
  // By normalising the first transformed normal (which is used by lighting calculations and needs
  // to be unit length), the same transform matrix can do double duty, scaling for emboss mapping,
  // and not scaling for lighting.
  v->normal_matrix = rslc_b_func(sb->b, "dolphin_normal_matrix", rslc_b_mat(sb->b, 3, 3), 0,
                                 nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, v->normal_matrix);
  result = rslc_b_local(sb->b, "result", rslc_b_mat(sb->b, 3, 3), 0, false);
  if (has_posmtx)
  {
    // Vertex format has a per-vertex matrix
    struct rslc_var* posidx = vb_posidx(v);
    struct rslc_var* normidx = rslc_b_local(
        sb->b, "normidx", sb->i[1], sb_bin(sb, RSLC_B_BAND, sb_ref(sb, posidx), sb_i(sb, 31)),
        false);
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_idx(sb, sb_ref(sb, result), sb_i(sb, k)),
             sb_swz(sb, vb_ui(v, SU_NORMALMATRICES, vb_plus(sb, normidx, k)), "xyz"));
  }
  else
  {
    // One shared matrix
    for (k = 0; k < 3; k++)
      sb_set(sb, sb_idx(sb, sb_ref(sb, result), sb_i(sb, k)),
             sb_swz(sb, vb_ui(v, SU_POSNORMALMATRIX, sb_i(sb, 3 + k)), "xyz"));
  }
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_ref(sb, result)));
  rslc_b_func_end(sb->b);
}

/* vec3(dot(coord, m[a]), dot(coord, m[a+1]), dot(coord, m[a+2]) or 1) */
uint32_t vb_texcoord_rows(const struct vs_build* v, struct rslc_var* coord, int m,
                          struct rslc_var* idx, u32 base, bool stq)
{
  const struct shader_build* sb = v->sb;
  uint32_t row[3];
  int k;
  for (k = 0; k < (stq ? 3 : 2); k++)
  {
    const uint32_t index =
        idx ? vb_plus(sb, idx, k) : sb_i(sb, static_cast<int32_t>(base) + k);
    row[k] = sb_call(sb, "dot", 2, sb_ref(sb, coord), vb_ui(v, m, index));
  }
  if (!stq)
    row[2] = sb_i(sb, 1);
  return sb_con(sb, sb->f[3], 3, row[0], row[1], row[2]);
}

bool vb_texcoord_transforms(struct vs_build* v)
{
  static const char* const fn_names[8] = {
      "dolphin_transform_texcoord0", "dolphin_transform_texcoord1", "dolphin_transform_texcoord2",
      "dolphin_transform_texcoord3", "dolphin_transform_texcoord4", "dolphin_transform_texcoord5",
      "dolphin_transform_texcoord6", "dolphin_transform_texcoord7"};
  static const char* const names[1] = {"coord"};
  static const unsigned dirs[1] = {RSLC_B_IN};
  const struct shader_build* sb = v->sb;
  const vertex_shader_uid_data* uid_data = v->uid_data;
  const struct rslc_type* types[1];
  u32 i;
  types[0] = sb->f[4];
  for (i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];
    struct rslc_func* f = rslc_b_func(sb->b, fn_names[i], sb->f[3], 1, types, dirs, names);
    struct rslc_var* coord = rslc_b_param(f, 0);
    v->transform_texcoord[i] = f;
    rslc_b_func_begin(sb->b, f);
    if (!texinfo.is_regular_texgen)
    {
      rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_con(sb, sb->f[3], 1,
                                                     sb_swz(sb, sb_ref(sb, coord), "xyz"))));
    }
    else
    {
      const bool stq = texinfo.regular.projection == TexSize::STQ;
      struct rslc_var* result = rslc_b_local(sb->b, "result", sb->f[3], 0, false);
      if ((uid_data->components & (VB_HAS_TEXMTXIDX0 << i)) != 0)
      {
        struct rslc_var* tmp;
        uint32_t index;
        if (v->expand)
        {
          /* the matrix index is the coordinate's third element, read where main's is */
          uint32_t zero = sb_i(sb, 0);
          struct rslc_var* in;
          if (texinfo.texcoord_elem_count != 3)
            return false;
          in = rslc_b_local(sb->b, "i", v->input_data,
                            rslc_b_call(sb->b, v->expand_data, &zero, 1), false);
          index = vb_mem(v, in, v->in_tex[i] + 2);
        }
        else
        {
          index = sb_swz(sb, sb_ref(sb, v->rawtex[i]), "z");
        }
        tmp = rslc_b_local(sb->b, "tmp", sb->i[1], sb_con(sb, sb->i[1], 1, index), false);
        sb_set(sb, sb_ref(sb, result),
               vb_texcoord_rows(v, coord, SU_TRANSFORMMATRICES, tmp, 0, stq));
      }
      else
      {
        sb_set(sb, sb_ref(sb, result),
               vb_texcoord_rows(v, coord, SU_TEXMATRICES, nullptr, 3 * i, stq));
      }
      // CHECKME: does this only work for regular tex gen types?
      if (uid_data->dualTexTrans_enabled)
      {
        static const char* const p_names[3] = {"P0", "P1", "P2"};
        const u32 postmtx_index = texinfo.regular.postmtx_index;
        struct rslc_var* p[3];
        uint32_t dots[3];
        int k;
        for (k = 0; k < 3; k++)
          p[k] = rslc_b_local(
              sb->b, p_names[k], sb->f[4],
              vb_ui(v, SU_POSTTRANSFORMMATRICES,
                    sb_i(sb, static_cast<int32_t>((postmtx_index + static_cast<u32>(k)) & 0x3f))),
              false);

        if (texinfo.regular.postmtx_normalize)
          sb_set(sb, sb_ref(sb, result), sb_call(sb, "normalize", 1, sb_ref(sb, result)));

        // multiply by postmatrix
        for (k = 0; k < 3; k++)
          dots[k] = sb_bin(sb, RSLC_B_ADD,
                           sb_call(sb, "dot", 2, sb_swz(sb, sb_ref(sb, p[k]), "xyz"),
                                   sb_ref(sb, result)),
                           sb_swz(sb, sb_ref(sb, p[k]), "w"));
        sb_set(sb, sb_ref(sb, result), sb_con(sb, sb->f[3], 3, dots[0], dots[1], dots[2]));
      }

      // When q is 0, the GameCube appears to have a special case
      // This can be seen in devkitPro's neheGX Lesson08 example for Wii
      // Makes differences in Rogue Squadron 3 (Hoth sky) and The Last Story (shadow culling)
      // TODO: check if this only affects XF_TEXGEN_REGULAR
      vb_if_set(v, sb_bin(sb, RSLC_B_EQ, sb_swz(sb, sb_ref(sb, result), "z"), sb_f(sb, 0.0)),
                sb_swz(sb, sb_ref(sb, result), "xy"),
                sb_call(sb, "clamp", 3,
                        sb_bin(sb, RSLC_B_DIV, sb_swz(sb, sb_ref(sb, result), "xy"),
                               sb_f(sb, 2.0)),
                        sb_con(sb, sb->f[2], 2, vb_negf(sb, 1.0), vb_negf(sb, 1.0)),
                        sb_con(sb, sb->f[2], 2, sb_f(sb, 1.0), sb_f(sb, 1.0))));
      rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_ref(sb, result)));
    }
    rslc_b_func_end(sb->b);
  }
  return true;
}

/* the body of dolphin_process_emulated_vertex */
void vb_vertex_body(const struct vs_build* v, struct rslc_var* vin, struct rslc_var* vout)
{
  const struct shader_build* sb = v->sb;
  const vertex_shader_uid_data* uid_data = v->uid_data;
  struct rslc_var* lighting[NUM_XF_COLOR_CHANNELS];
  u32 chan, i;

#define VIN(k) vb_mem(v, vin, k)
#define VOUT(k) vb_mem(v, vout, k)
  sb_set(sb, VOUT(VOUT_POSITION),
         sb_con(sb, sb->f[4], 2,
                sb_bin(sb, RSLC_B_MUL, VIN(VIN_POSITION),
                       rslc_b_call(sb->b, v->position_matrix, nullptr, 0)),
                sb_f(sb, 1.0)));
  sb_set(sb, VOUT(VOUT_NORMAL),
         sb_call(sb, "normalize", 1,
                 sb_bin(sb, RSLC_B_MUL, VIN(VIN_NORMAL),
                        rslc_b_call(sb->b, v->normal_matrix, nullptr, 0))));

  for (chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
  {
    static const char* const names[NUM_XF_COLOR_CHANNELS] = {"vertex_lighting_0",
                                                             "vertex_lighting_1"};
    uint32_t args[3];
    args[0] = VIN(VIN_COLOR_0 + static_cast<int>(chan));
    args[1] = sb_swz(sb, VOUT(VOUT_POSITION), "xyz");
    args[2] = VOUT(VOUT_NORMAL);
    lighting[chan] = rslc_b_local(sb->b, names[chan], sb->f[4],
                                  rslc_b_call(sb->b, v->lighting[chan], args, 3), false);
    sb_set(sb, VOUT(VOUT_COLOR_0 + static_cast<int>(chan)), sb_ref(sb, lighting[chan]));
  }

  if (v->host_config->per_pixel_lighting)
  {
    // When per-pixel lighting is enabled, the vertex colors are passed through
    // unmodified so we can evaluate the lighting in the pixel shader.
    sb_set(sb, VOUT(VOUT_COLOR_0), VIN(VIN_COLOR_0));
    sb_set(sb, VOUT(VOUT_COLOR_1), VIN(VIN_COLOR_1));
  }
  else
  {
    // The number of colors available to TEV is determined by numColorChans.
    // We have to provide the fields to match the interface, so set to zero if it's not enabled.
    if (uid_data->numColorChans == 0)
      sb_set(sb, VOUT(VOUT_COLOR_0),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));
    if (uid_data->numColorChans <= 1)
      sb_set(sb, VOUT(VOUT_COLOR_1),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));
  }

  for (i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];
    const int tc = VOUT_TEXTURE_COORD_0 + static_cast<int>(i);
    const auto texgentype =
        texinfo.is_regular_texgen ? TexGenType::Regular : texinfo.other.texgentype;

    switch (texgentype)
    {
    case TexGenType::EmbossMap:  // calculate tex coords into bump map
    {
      struct rslc_var *ldir, *tangent, *binormal;
      uint32_t body;
      rslc_b_begin_block(sb->b);
      // transform the light dir into tangent space
      ldir = rslc_b_local(
          sb->b, "ldir", sb->f[3],
          sb_call(sb, "normalize", 1,
                  sb_bin(sb, RSLC_B_SUB,
                         sb_swz(sb,
                                sb_mem(sb,
                                       vb_ui(v, SU_LIGHTS,
                                             sb_i(sb, texinfo.other.emboss_lightshift)),
                                       3),
                                "xyz"),
                         sb_swz(sb, VOUT(VOUT_POSITION), "xyz"))),
          false);
      tangent = rslc_b_local(sb->b, "tangent", sb->f[3],
                             sb_bin(sb, RSLC_B_MUL, VIN(VIN_TANGENT),
                                    rslc_b_call(sb->b, v->normal_matrix, nullptr, 0)),
                             false);
      binormal = rslc_b_local(sb->b, "binormal", sb->f[3],
                              sb_bin(sb, RSLC_B_MUL, VIN(VIN_BINORMAL),
                                     rslc_b_call(sb->b, v->normal_matrix, nullptr, 0)),
                              false);
      sb_set(sb, sb_swz(sb, VOUT(tc), "xyz"),
             sb_bin(sb, RSLC_B_ADD,
                    sb_swz(sb, VOUT(VOUT_TEXTURE_COORD_0 + texinfo.other.emboss_sourceshift),
                           "xyz"),
                    sb_con(sb, sb->f[3], 3,
                           sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, tangent)),
                           sb_call(sb, "dot", 2, sb_ref(sb, ldir), sb_ref(sb, binormal)),
                           sb_f(sb, 0.0))));
      body = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, body);
      break;
    }
    case TexGenType::Color0:
    case TexGenType::Color1:
    {
      struct rslc_var* l = lighting[texgentype == TexGenType::Color0 ? 0 : 1];
      sb_set(sb, sb_swz(sb, VOUT(tc), "xyz"),
             sb_con(sb, sb->f[3], 3, sb_swz(sb, sb_ref(sb, l), "x"),
                    sb_swz(sb, sb_ref(sb, l), "y"), sb_i(sb, 1)));
      break;
    }
    case TexGenType::Regular:
    {
      uint32_t arg = VIN(VIN_TEXTURE_COORD_0 + static_cast<int>(i));
      sb_set(sb, VOUT(tc), rslc_b_call(sb->b, v->transform_texcoord[i], &arg, 1));
      break;
    }
    }
  }

  // Fill out output that is unused
  for (i = uid_data->numTexGens; i < 8; i++)
    sb_set(sb, VOUT(VOUT_TEXTURE_COORD_0 + static_cast<int>(i)),
           sb_con(sb, sb->f[3], 3, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
#undef VOUT
#undef VIN
}
}  // namespace

bool BuildVertexShader(struct shader_build* sb, const void* arg)
{
  const VertexShaderBuildArgs* a = static_cast<const VertexShaderBuildArgs*>(arg);
  const ShaderHostConfig& host_config = *a->host_config;
  const vertex_shader_uid_data* uid_data = a->uid_data;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool per_pixel_lighting = host_config.per_pixel_lighting;
  const u32 components = uid_data->components;
  static const char* const tex_names[8] = {"tex0", "tex1", "tex2", "tex3",
                                           "tex4", "tex5", "tex6", "tex7"};
  struct vs_build v;
  struct vs_output_layout plain;
  struct rslc_b_quals q;
  const struct rslc_type* types[48];
  const char* names[48];
  const struct rslc_type *light, *vs_output, *vin_t, *vout_t;
  struct rslc_var *vs = nullptr, *varying[20];
  struct rslc_func *emulated, *process, *fmain;
  struct rslc_var *i_var = nullptr, *o, *vertex_color[2], *vin, *vout;
  struct rslc_var *clip_dist0 = nullptr, *clip_dist1 = nullptr;
  u32 n, k, i;

  memset(&v, 0, sizeof(v));
  v.sb = sb;
  v.uid_data = uid_data;
  v.host_config = &host_config;
  v.api_type = a->api_type;
  v.expand = uid_data->vs_expand != VSExpand::None;
  memset(varying, 0, sizeof(varying));

  light = BuildLightStruct(sb);

  // uniforms
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 2);
  n = BuildShaderUniforms(sb, light, types, names, 0);
  rslc_b_block_q(sb->b, &q, "VSBlock", nullptr, types, names, n, v.u);

  if (v.expand)
  {
    rslc_b_quals_init(&q);
    sb_ubo(sb, &q, 4);
    n = BuildGeometryShaderUniforms(sb, types, names, 0);
    rslc_b_block_q(sb->b, &q, "GSBlock", nullptr, types, names, n, v.gs);

    if (a->api_type == APIType::D3D)
    {
      // D3D doesn't include the base vertex in SV_VertexID
      types[0] = sb->u[1];
      names[0] = "base_vertex";
      rslc_b_quals_init(&q);
      sb_ubo(sb, &q, 5);
      rslc_b_block_q(sb->b, &q, "DX_Constants", nullptr, types, names, 1, &v.base_vertex);
    }
  }

  BuildVSOutputLayout(sb, uid_data->numTexGens, host_config, 0, &plain);
  vs_output = rslc_b_struct(sb->b, "VS_OUTPUT", plain.types, plain.names, plain.n);

  v.isnan_fn = BuildIsNanHeader(sb, a->api_type);
  BuildLightingShaderHeader(sb, uid_data->lighting, v.u[SU_MATERIALS], v.u[SU_LIGHTS],
                            v.lighting);

  if (!v.expand)
  {
    static const char* const raw_names[3] = {"rawnormal", "rawtangent", "rawbinormal"};
    static const char* const rawtex_names[8] = {"rawtex0", "rawtex1", "rawtex2", "rawtex3",
                                                "rawtex4", "rawtex5", "rawtex6", "rawtex7"};
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::Position));
    v.rawpos = rslc_b_declare(sb->b, &q, "rawpos", sb->f[4], 0);
    if ((components & VB_HAS_POSMTXIDX) != 0)
    {
      q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::PositionMatrix));
      v.posmtx = rslc_b_declare(sb->b, &q, "posmtx", sb->u[4], 0);
    }
    for (k = 0; k < 3; k++)
    {
      if ((components & (VB_HAS_NORMAL << k)) != 0)
      {
        q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::Normal + k));
        v.raw[k] = rslc_b_declare(sb->b, &q, raw_names[k], sb->f[3], 0);
      }
    }
    for (k = 0; k < 2; k++)
    {
      static const char* const color_names[2] = {"rawcolor0", "rawcolor1"};
      if ((components & (VB_HAS_COL0 << k)) != 0)
      {
        q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::Color0 + k));
        v.rawcolor[k] = rslc_b_declare(sb->b, &q, color_names[k], sb->f[4], 0);
      }
    }
    for (i = 0; i < 8; ++i)
    {
      const u32 has_texmtx = (components & (VB_HAS_TEXMTXIDX0 << i));
      if (uid_data->texGenInfo[i].texcoord_elem_count != 0 || has_texmtx != 0)
      {
        q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::TexCoord0 + i));
        v.rawtex[i] = rslc_b_declare(sb->b, &q, rawtex_names[i], sb->f[has_texmtx != 0 ? 3 : 2],
                                     0);
      }
    }
  }
  else
  {
    static const char* const unpack_names[1] = {"value"};
    static const unsigned dirs[1] = {RSLC_B_IN};
    static const char* const member_names[] = {
        "posmtx",   "pos0",     "pos1",     "pos2",     "normal0",   "normal1",   "normal2",
        "tangent0", "tangent1", "tangent2", "binormal0", "binormal1", "binormal2", "color0",
        "color1",   "tex0",     "tex1",     "tex2",     "tex3",      "tex4",      "tex5",
        "tex6",     "tex7"};
    static const char* const tex_member_names[8][3] = {
        {"tex0_0", "tex0_1", "tex0_2"}, {"tex1_0", "tex1_1", "tex1_2"},
        {"tex2_0", "tex2_1", "tex2_2"}, {"tex3_0", "tex3_1", "tex3_2"},
        {"tex4_0", "tex4_1", "tex4_2"}, {"tex5_0", "tex5_1", "tex5_2"},
        {"tex6_0", "tex6_1", "tex6_2"}, {"tex7_0", "tex7_1", "tex7_2"}};
    const struct rslc_type* unpack_types[1];
    struct rslc_var* value;
    uint32_t parts[4];

    // Can't use float3, etc because we want 4-byte alignment
    unpack_types[0] = sb->u[1];
    v.unpack = rslc_b_func(sb->b, "unpack_ubyte4", sb->u[4], 1, unpack_types, dirs, unpack_names);
    rslc_b_func_begin(sb->b, v.unpack);
    value = rslc_b_param(v.unpack, 0);
    parts[0] = sb_bin(sb, RSLC_B_BAND, sb_ref(sb, value), sb_u(sb, 0xffu));
    for (k = 1; k < 3; k++)
      parts[k] = sb_bin(sb, RSLC_B_BAND,
                        sb_bin(sb, RSLC_B_SHR, sb_ref(sb, value),
                               sb_i(sb, static_cast<int32_t>(8 * k))),
                        sb_u(sb, 0xffu));
    parts[3] = sb_bin(sb, RSLC_B_SHR, sb_ref(sb, value), sb_i(sb, 24));
    rslc_b_stmt(sb->b,
                rslc_b_return(sb->b, sb_con(sb, sb->u[4], 4, parts[0], parts[1], parts[2],
                                            parts[3])));
    rslc_b_func_end(sb->b);

    n = 0;
    v.in_posmtx = v.in_pos = -1;
    if (components & VB_HAS_POSMTXIDX)
    {
      // Note: posmtx is handled in the matrix transform functions
      v.in_posmtx = static_cast<int>(n);
      types[n] = sb->u[1];
      names[n++] = member_names[0];
    }
    v.in_pos = static_cast<int>(n);
    for (k = 0; k < (uid_data->position_has_3_elems ? 3u : 2u); k++)
    {
      types[n] = sb->f[1];
      names[n++] = member_names[1 + k];
    }
    for (k = 0; k < 3; k++)
    {
      v.in_raw[k] = -1;
      if (components & (VB_HAS_NORMAL << k))
      {
        u32 c;
        v.in_raw[k] = static_cast<int>(n);
        for (c = 0; c < 3; c++)
        {
          types[n] = sb->f[1];
          names[n++] = member_names[4 + 3 * k + c];
        }
      }
    }
    for (k = 0; k < 2; k++)
    {
      v.in_color[k] = -1;
      if (components & (VB_HAS_COL0 << k))
      {
        v.in_color[k] = static_cast<int>(n);
        types[n] = sb->u[1];
        names[n++] = member_names[13 + k];
      }
    }
    for (k = 0; k < 8; k++)
    {
      const u32 count = uid_data->texGenInfo[k].texcoord_elem_count;
      u32 c;
      v.in_tex[k] = static_cast<int>(n);
      if (count == 1)
      {
        types[n] = sb->f[1];
        names[n++] = member_names[15 + k];
      }
      else
      {
        for (c = 0; c < count; c++)
        {
          types[n] = sb->f[1];
          names[n++] = tex_member_names[k][c];
        }
      }
    }
    v.input_data = rslc_b_struct(sb->b, "InputData", types, names, n);

    types[0] = rslc_b_array(sb->b, v.input_data, 0);
    names[0] = "input_buffer";
    rslc_b_quals_init(&q);
    sb_ssbo(sb, &q, 1);
    q.access = RSLC_B_READONLY;
    rslc_b_block_q(sb->b, &q, "InputBuffer", nullptr, types, names, 1, &v.input_buffer);
  }

  if (host_config.backend_geometry_shaders)
  {
    struct vs_output_layout out_l;
    BuildVSOutputLayout(sb, uid_data->numTexGens, host_config,
                        BuildInterpolationQuals(msaa, ssaa), &out_l);
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
#define VS_VARYING(idx, name, type)                                                                \
  do                                                                                               \
  {                                                                                                \
    q.location = sb_varying_location(sb, counter++);                                              \
    varying[idx] = rslc_b_declare(sb->b, &q, name, type, 0);                                      \
  } while (0)
    VS_VARYING(plain.colors_0, "colors_0", sb->f[4]);
    VS_VARYING(plain.colors_1, "colors_1", sb->f[4]);
    for (i = 0; i < uid_data->numTexGens; ++i)
      VS_VARYING(plain.tex0 + static_cast<int>(i), tex_names[i], sb->f[3]);
    if (!host_config.fast_depth_calc)
      VS_VARYING(plain.clipPos, "clipPos", sb->f[4]);
    if (per_pixel_lighting)
    {
      VS_VARYING(plain.Normal, "Normal", sb->f[3]);
      VS_VARYING(plain.WorldPos, "WorldPos", sb->f[3]);
    }
#undef VS_VARYING
  }

  // Note: this is done after to ensure above global variables are accessible
  if (v.expand)
    vb_expand_data(&v);
  vb_transform_matrices(&v);
  if (!vb_texcoord_transforms(&v))
    return false;

  {
    static const char* const vin_names[14] = {
        "color_0",         "color_1",         "position",        "normal",
        "binormal",        "tangent",         "texture_coord_0", "texture_coord_1",
        "texture_coord_2", "texture_coord_3", "texture_coord_4", "texture_coord_5",
        "texture_coord_6", "texture_coord_7"};
    static const char* const vout_names[12] = {
        "color_0",         "color_1",         "position",        "normal",
        "texture_coord_0", "texture_coord_1", "texture_coord_2", "texture_coord_3",
        "texture_coord_4", "texture_coord_5", "texture_coord_6", "texture_coord_7"};
    static const char* const param_names[2] = {"vertex_input", "vertex_output"};
    static const unsigned dirs[2] = {RSLC_B_IN, RSLC_B_OUT};
    const struct rslc_type* param_types[2];
    uint32_t args[2];

    types[0] = types[1] = types[2] = sb->f[4];
    types[3] = types[4] = types[5] = sb->f[3];
    for (k = 0; k < 8; k++)
      types[6 + k] = sb->f[4];
    vin_t = rslc_b_struct(sb->b, "DolphinVertexInput", types, vin_names, 14);
    types[3] = sb->f[3];
    for (k = 0; k < 8; k++)
      types[4 + k] = sb->f[3];
    vout_t = rslc_b_struct(sb->b, "DolphinVertexOutput", types, vout_names, 12);

    param_types[0] = vin_t;
    param_types[1] = vout_t;
    emulated = rslc_b_func(sb->b, "dolphin_process_emulated_vertex", sb->v, 2, param_types,
                           dirs, param_names);
    rslc_b_func_begin(sb->b, emulated);
    vb_vertex_body(&v, rslc_b_param(emulated, 0), rslc_b_param(emulated, 1));
    rslc_b_func_end(sb->b);

    process = rslc_b_func(sb->b, "process_vertex", sb->v, 2, param_types, dirs, param_names);
    rslc_b_func_begin(sb->b, process);
    args[0] = sb_ref(sb, rslc_b_param(process, 0));
    args[1] = sb_ref(sb, rslc_b_param(process, 1));
    vb_call_stmt(&v, emulated, args, 2);
    rslc_b_func_end(sb->b);
  }

  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);

  if (v.expand)
  {
    static const char* const extract_names[3] = {"rawnormal", "rawtangent", "rawbinormal"};
    static const char* const color_names[2] = {"rawcolor0", "rawcolor1"};
    static const char* const rawtex_names[8] = {"rawtex0", "rawtex1", "rawtex2", "rawtex3",
                                                "rawtex4", "rawtex5", "rawtex6", "rawtex7"};
    uint32_t zero = sb_i(sb, 0);
#define INPUT_AT(k) vb_mem(&v, i_var, static_cast<int>(k))
    i_var = rslc_b_local(sb->b, "i", v.input_data, rslc_b_call(sb->b, v.expand_data, &zero, 1),
                         false);
    v.rawpos = rslc_b_local(
        sb->b, "rawpos", sb->f[4],
        sb_con(sb, sb->f[4], 4, INPUT_AT(v.in_pos), INPUT_AT(v.in_pos + 1),
               uid_data->position_has_3_elems ? INPUT_AT(v.in_pos + 2) : sb_f(sb, 0.0),
               sb_f(sb, 1.0)),
        false);
    for (k = 0; k < 3; k++)
    {
      if (v.in_raw[k] >= 0)
        v.raw[k] = rslc_b_local(sb->b, extract_names[k], sb->f[3],
                                sb_con(sb, sb->f[3], 3, INPUT_AT(v.in_raw[k]),
                                       INPUT_AT(v.in_raw[k] + 1), INPUT_AT(v.in_raw[k] + 2)),
                                false);
    }
    for (k = 0; k < 2; k++)
    {
      if (v.in_color[k] >= 0)
      {
        uint32_t packed = INPUT_AT(v.in_color[k]);
        v.rawcolor[k] = rslc_b_local(
            sb->b, color_names[k], sb->f[4],
            sb_bin(sb, RSLC_B_DIV,
                   sb_con(sb, sb->f[4], 1, rslc_b_call(sb->b, v.unpack, &packed, 1)),
                   sb_f(sb, 255.0)),
            false);
      }
    }
    for (k = 0; k < 8; k++)
    {
      const int t = v.in_tex[k];
      uint32_t e;
      switch (uid_data->texGenInfo[k].texcoord_elem_count)
      {
      case 1:
        e = sb_con(sb, sb->f[3], 3, INPUT_AT(t), sb_f(sb, 0.0), sb_f(sb, 0.0));
        break;
      case 2:
        e = sb_con(sb, sb->f[3], 3, INPUT_AT(t), INPUT_AT(t + 1), sb_f(sb, 0.0));
        break;
      case 3:
        e = sb_con(sb, sb->f[3], 3, INPUT_AT(t), INPUT_AT(t + 1), INPUT_AT(t + 2));
        break;
      default:
        continue;
      }
      v.rawtex[k] = rslc_b_local(sb->b, rawtex_names[k], sb->f[3], e, false);
    }
#undef INPUT_AT
  }

  o = rslc_b_local(sb->b, "o", vs_output, 0, false);

  // xfmem.numColorChans controls the number of color channels available to TEV, but we still need
  // to generate all channels here, as it can be used in texgen. Cel-damage is an example of this.
  vertex_color[0] = rslc_b_local(sb->b, "vertex_color_0", sb->f[4], 0, false);
  vertex_color[1] = rslc_b_local(sb->b, "vertex_color_1", sb->f[4], 0, false);

  // To use color 1, the vertex descriptor must have color 0 and 1.
  // If color 1 is present but not color 0, it is used for lighting channel 0.
  {
    const bool use_color_1 =
        (components & (VB_HAS_COL0 | VB_HAS_COL1)) == (VB_HAS_COL0 | VB_HAS_COL1);
    u32 color;
    for (color = 0; color < NUM_XF_COLOR_CHANNELS; color++)
    {
      uint32_t e;
      if ((color == 0 || use_color_1) && (components & (VB_HAS_COL0 << color)) != 0)
        e = sb_ref(sb, v.rawcolor[color]);  // color0 for channel 0, color1 for channel 1
      else if (color == 0 && (components & VB_HAS_COL1) != 0)
        e = sb_ref(sb, v.rawcolor[1]);  // color1 for channel 0 if color0 is not present
      else
        e = sb_ref(sb, v.u[SU_MISSING_COLOR_VALUE]);
      sb_set(sb, sb_ref(sb, vertex_color[color]), e);
    }
  }

  vin = rslc_b_local(sb->b, "vertex_input", vin_t, 0, false);
#define VIN(k) vb_mem(&v, vin, k)
  sb_set(sb, VIN(VIN_COLOR_0), sb_ref(sb, vertex_color[0]));
  sb_set(sb, VIN(VIN_COLOR_1), sb_ref(sb, vertex_color[1]));
  sb_set(sb, VIN(VIN_POSITION), sb_ref(sb, v.rawpos));
  {
    static const u32 bits[3] = {VB_HAS_NORMAL, VB_HAS_BINORMAL, VB_HAS_TANGENT};
    static const int raws[3] = {RAW_NORMAL, RAW_BINORMAL, RAW_TANGENT};
    static const int cached[3] = {SU_CACHED_NORMAL, SU_CACHED_BINORMAL, SU_CACHED_TANGENT};
    for (k = 0; k < 3; k++)
    {
      sb_set(sb, VIN(VIN_NORMAL + static_cast<int>(k)),
             (components & bits[k]) != 0 ? sb_ref(sb, v.raw[raws[k]]) :
                                           sb_swz(sb, vb_u(&v, cached[k]), "xyz"));
    }
  }

  for (i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];
    struct rslc_var *coord, *raw = nullptr;
    rslc_b_begin_block(sb->b);
    coord = rslc_b_local(
        sb->b, "coord", sb->f[4],
        sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 1.0), sb_f(sb, 1.0)),
        false);
    switch (texinfo.sourcerow)
    {
    case SourceRow::Geom:
      raw = v.rawpos;
      break;
    case SourceRow::Normal:
      if ((components & VB_HAS_NORMAL) != 0)
        raw = v.raw[RAW_NORMAL];
      break;
    case SourceRow::Colors:
      ASSERT(!texinfo.is_regular_texgen);
      ASSERT(texinfo.other.texgentype == TexGenType::Color0 ||
             texinfo.other.texgentype == TexGenType::Color1);
      break;
    case SourceRow::BinormalT:
      if ((components & VB_HAS_TANGENT) != 0)
        raw = v.raw[RAW_TANGENT];
      break;
    case SourceRow::BinormalB:
      if ((components & VB_HAS_BINORMAL) != 0)
        raw = v.raw[RAW_BINORMAL];
      break;
    default:
    {
      const u32 texnum = static_cast<u32>(texinfo.sourcerow) - static_cast<u32>(SourceRow::Tex0);
      if (uid_data->texGenInfo[texnum].texcoord_elem_count != 0)
      {
        sb_set(sb, sb_ref(sb, coord),
               sb_con(sb, sb->f[4], 4, sb_swz(sb, sb_ref(sb, v.rawtex[texnum]), "x"),
                      sb_swz(sb, sb_ref(sb, v.rawtex[texnum]), "y"), sb_f(sb, 1.0),
                      sb_f(sb, 1.0)));
      }
      break;
    }
    }
    if (raw)
      sb_set(sb, sb_swz(sb, sb_ref(sb, coord), "xyz"), sb_swz(sb, sb_ref(sb, raw), "xyz"));
    // Input form of AB11 sets z element to 1.0
    if (texinfo.inputform == TexInputForm::AB11)
      sb_set(sb, sb_swz(sb, sb_ref(sb, coord), "z"), sb_f(sb, 1.0));

    // Convert NaNs to 1 - needed to fix eyelids in Shadow the Hedgehog during cutscenes
    // See https://bugs.dolphin-emu.org/issues/11458
    for (k = 0; k < 3; k++)
    {
      static const char* const c[3] = {"x", "y", "z"};
      vb_if_set(&v, sb_isnan(sb, v.isnan_fn, sb_swz(sb, sb_ref(sb, coord), c[k])),
                sb_swz(sb, sb_ref(sb, coord), c[k]), sb_f(sb, 1.0));
    }
    sb_set(sb, VIN(VIN_TEXTURE_COORD_0 + static_cast<int>(i)), sb_ref(sb, coord));
    rslc_b_stmt(sb->b, rslc_b_end_block(sb->b));
  }

  // Initialize other texture coordinates that are unused
  for (i = uid_data->numTexGens; i < 8; i++)
    sb_set(sb, VIN(VIN_TEXTURE_COORD_0 + static_cast<int>(i)),
           sb_con(sb, sb->f[4], 4, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
#undef VIN

  vout = rslc_b_local(sb->b, "vertex_output", vout_t, 0, false);
  {
    uint32_t args[2];
    args[0] = sb_ref(sb, vin);
    args[1] = sb_ref(sb, vout);
    vb_call_stmt(&v, process, args, 2);
  }

#define O(k) vb_mem(&v, o, k)
#define VOUT(k) vb_mem(&v, vout, k)
  {
    uint32_t d[4];
    for (k = 0; k < 4; k++)
      d[k] = sb_call(sb, "dot", 2, vb_ui(&v, SU_PROJECTION, sb_i(sb, static_cast<int32_t>(k))),
                     VOUT(VOUT_POSITION));
    sb_set(sb, O(plain.pos), sb_con(sb, sb->f[4], 4, d[0], d[1], d[2], d[3]));
  }
  for (i = 0; i < uid_data->numTexGens; ++i)
    sb_set(sb, O(plain.tex0 + static_cast<int>(i)),
           VOUT(VOUT_TEXTURE_COORD_0 + static_cast<int>(i)));
  sb_set(sb, O(plain.colors_0), VOUT(VOUT_COLOR_0));
  sb_set(sb, O(plain.colors_1), VOUT(VOUT_COLOR_1));
  if (per_pixel_lighting)
  {
    sb_set(sb, O(plain.Normal), VOUT(VOUT_NORMAL));
    // TODO: Rename, this is actually in Viewspace...
    sb_set(sb, O(plain.WorldPos), sb_swz(sb, VOUT(VOUT_POSITION), "xyz"));
  }
#undef VOUT

  if (v.expand)
  {
    struct vs_expansion x;
    /* (gl_VertexID & bit) != 0 */
#define VID_BIT(bit)                                                                               \
  sb_bin(sb, RSLC_B_NE, sb_bin(sb, RSLC_B_BAND, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, bit)),       \
         sb_i(sb, 0))
    memset(&x, 0, sizeof(x));
    x.o = o;
    x.layout = &plain;
    x.projection = v.u[SU_PROJECTION];
    x.clinept = v.gs[1];
    x.texoffset = v.gs[2];
    x.is_bottom = rslc_b_local(sb->b, "is_bottom", sb->bl, VID_BIT(2), false);
    if (uid_data->vs_expand == VSExpand::Line)
    {
      struct rslc_var *id_offset, *other;
      uint32_t then_s, else_s, other_arg;
      // Line expansion
      id_offset = rslc_b_local(sb->b, "id_offset", sb->i[1], sb_i(sb, 0), false);
      rslc_b_begin_block(sb->b);
      sb_op_set(sb, RSLC_B_SUB, sb_ref(sb, id_offset), sb_i(sb, 1));
      then_s = rslc_b_end_block(sb->b);
      rslc_b_begin_block(sb->b);
      sb_op_set(sb, RSLC_B_ADD, sb_ref(sb, id_offset), sb_i(sb, 1));
      else_s = rslc_b_end_block(sb->b);
      rslc_b_stmt(sb->b, rslc_b_if(sb->b, sb_ref(sb, x.is_bottom), then_s, else_s));
      other_arg = sb_ref(sb, id_offset);
      other = rslc_b_local(sb->b, "other", v.input_data,
                           rslc_b_call(sb->b, v.expand_data, &other_arg, 1), false);
#define OTHER(k) vb_mem(&v, other, k)
      x.other_pos = rslc_b_local(
          sb->b, "other_pos", sb->f[4],
          sb_con(sb, sb->f[4], 4, OTHER(v.in_pos), OTHER(v.in_pos + 1),
                 uid_data->position_has_3_elems ? OTHER(v.in_pos + 2) : sb_f(sb, 0.0),
                 sb_f(sb, 1.0)),
          false);
      if (components & VB_HAS_POSMTXIDX)
      {
        static const char* const p_names[3] = {"other_p0", "other_p1", "other_p2"};
        struct rslc_var *posidx, *p[3];
        posidx = rslc_b_local(sb->b, "other_posidx", sb->u[1],
                              sb_bin(sb, RSLC_B_BAND, OTHER(v.in_posmtx), sb_i(sb, 0xff)), false);
        for (k = 0; k < 3; k++)
          p[k] = rslc_b_local(sb->b, p_names[k], sb->f[4],
                              vb_ui(&v, SU_TRANSFORMMATRICES,
                                    vb_plus(sb, posidx, static_cast<int>(k))),
                              false);
        sb_set(sb, sb_ref(sb, x.other_pos),
               sb_con(sb, sb->f[4], 4,
                      sb_call(sb, "dot", 2, sb_ref(sb, p[0]), sb_ref(sb, x.other_pos)),
                      sb_call(sb, "dot", 2, sb_ref(sb, p[1]), sb_ref(sb, x.other_pos)),
                      sb_call(sb, "dot", 2, sb_ref(sb, p[2]), sb_ref(sb, x.other_pos)),
                      sb_f(sb, 1.0)));
      }
      else
      {
        sb_set(sb, sb_ref(sb, x.other_pos),
               sb_con(sb, sb->f[4], 2,
                      sb_bin(sb, RSLC_B_MUL, sb_ref(sb, x.other_pos),
                             rslc_b_call(sb->b, v.position_matrix, nullptr, 0)),
                      sb_f(sb, 1.0)));
      }
#undef OTHER
      x.is_right = rslc_b_local(sb->b, "is_right", sb->bl, VID_BIT(1), false);
      BuildVSLineExpansion(sb, &x);
    }
    else if (uid_data->vs_expand == VSExpand::Point)
    {
      // Point expansion
      x.is_right = rslc_b_local(sb->b, "is_right", sb->bl, VID_BIT(1), false);
      BuildVSPointExpansion(sb, &x);
    }
#undef VID_BIT
  }

  // clipPos/w needs to be done in pixel shader, not here
  if (!host_config.fast_depth_calc)
    sb_set(sb, O(plain.clipPos), O(plain.pos));

#define OP(c) sb_swz(sb, O(plain.pos), c)
#define CC(c) sb_swz(sb, vb_u(&v, SU_PIXELCENTERCORRECTION), c)
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
  else
  {
    // Same depth adjustment for Sonic. Without depth clamping, it unfortunately
    // affects non-clipping uses of depth too.
    sb_set(sb, OP("z"),
           sb_bin(sb, RSLC_B_MUL, OP("z"), sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), sb_f(sb, 1e-7))));
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
                           sb_con(sb, sb->f[2], 2, sb_f(sb, 1.0), vb_negf(sb, 1.0)))));

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
    // By now our position is in clip space
    // however, higher resolutions than the Wii outputs
    // cause an additional pixel offset
    // due to a higher pixel density
    // we need to correct this by converting our
    // clip-space position into the Wii's screen-space
    // acquire the right pixel and then convert it back
#define HALF_VP(c)                                                                                 \
  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, vb_u(&v, SU_VIEWPORT_SIZE), c), sb_f(sb, 0.5))
    struct rslc_var *ss_x, *ss_y;
    uint32_t then_s;
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
    struct sb_place vs_place = {vs, nullptr, -1}, o_place = {o, nullptr, -1};
    BuildAssignVSOutputMembers(sb, &plain, &vs_place, &o_place, host_config);
  }
  else
  {
    // TODO: Pass interface blocks between shader stages even if geometry shaders
    // are not supported, however that will require at least OpenGL 3.2 support.
    for (i = 0; i < uid_data->numTexGens; ++i)
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

