// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderGen.h"

#include <cstring>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_in_map{1u, 2u, 3u, 3u};
constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_out_map{4u, 4u, 4u, 3u};

bool geometry_shader_uid_data::IsPassthrough() const
{
  const bool stereo = g_ActiveConfig.stereo_mode != StereoMode::Off;
  const bool wireframe = g_ActiveConfig.bWireFrame;
  return primitive_type >= static_cast<u32>(PrimitiveType::Triangles) && !stereo && !wireframe;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  GeometryShaderUid out;

  geometry_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->primitive_type = static_cast<u32>(primitive_type);
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;

  return out;
}

/* ---- the stage built by calls ---- */

namespace
{
struct gs_build
{
  const struct shader_build* sb;
  const GeometryShaderBuildArgs* a;
  const struct vs_output_layout* layout;
  struct rslc_var *ps, *layer, *eye, *i, *first, *gsblock[4];
  bool wireframe, stereo;
};

uint32_t gs_member(const struct gs_build* g, struct rslc_var* v, int idx)
{
  return sb_mem(g->sb, sb_ref(g->sb, v), static_cast<uint32_t>(idx));
}

void gs_emit_vertex(struct gs_build* g, struct rslc_var* v, bool first_vertex)
{
  const struct shader_build* sb = g->sb;
  const ShaderHostConfig& host = *g->a->host_config;
  struct sb_place ps_place = {g->ps, nullptr, -1};
  struct sb_place v_place = {v, nullptr, -1};
  uint32_t pos;

  if (g->wireframe && first_vertex)
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, sb_ref(sb, g->i), sb_i(sb, 0)),
                          rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, sb_ref(sb, g->first),
                                                           sb_ref(sb, v))),
                          0));

  // Vulkan NDC space has Y pointing down (right-handed NDC space).
  pos = g->layout->pos;
  if (g->a->api_type == APIType::Vulkan)
    sb_set(sb, sb_bi(sb, "gl_Position"),
           sb_con(sb, sb->f[4], 4, sb_swz(sb, gs_member(g, v, pos), "x"),
                  sb_un(sb, RSLC_B_NEG, sb_swz(sb, gs_member(g, v, pos), "y")),
                  sb_swz(sb, gs_member(g, v, pos), "z"), sb_swz(sb, gs_member(g, v, pos), "w")));
  else
    sb_set(sb, sb_bi(sb, "gl_Position"), gs_member(g, v, pos));

  if (host.backend_depth_clamp)
  {
    sb_set(sb, sb_idx(sb, sb_bi(sb, "gl_ClipDistance"), sb_i(sb, 0)),
           gs_member(g, v, g->layout->clipDist0));
    sb_set(sb, sb_idx(sb, sb_bi(sb, "gl_ClipDistance"), sb_i(sb, 1)),
           gs_member(g, v, g->layout->clipDist1));
  }
  BuildAssignVSOutputMembers(sb, g->layout, &ps_place, &v_place, host);

  if (g->stereo)
  {
    // Select the output layer
    sb_set(sb, sb_bi(sb, "gl_Layer"), sb_ref(sb, g->eye));
    if (!host.backend_gl_layer_in_fs)
      sb_set(sb, sb_ref(sb, g->layer), sb_ref(sb, g->eye));
  }
  sb_do(sb, sb_call(sb, "EmitVertex", 0));
}

/* v.pos */
uint32_t gs_pos_of(const struct shader_build* sb, const void* ctx)
{
  const std::pair<struct rslc_var*, int>* p = static_cast<const std::pair<struct rslc_var*, int>*>(ctx);
  return sb_mem(sb, sb_ref(sb, p->first), static_cast<uint32_t>(p->second));
}

/* (((ctexoffset[k] >> i) & 0x1) != 0) */
uint32_t gs_tex_bit(const struct gs_build* g, int k, u32 i)
{
  const struct shader_build* sb = g->sb;
  return sb_bin(sb, RSLC_B_NE,
                sb_bin(sb, RSLC_B_BAND,
                       sb_bin(sb, RSLC_B_SHR, sb_idx(sb, sb_ref(sb, g->gsblock[2]), sb_i(sb, k)),
                              sb_i(sb, static_cast<int32_t>(i))),
                       sb_i(sb, 0x1)),
                sb_i(sb, 0));
}

/* v.texN.c op= e */
void gs_tex_add(const struct gs_build* g, struct rslc_var* v, u32 n, const char* c, uint32_t e)
{
  const struct shader_build* sb = g->sb;
  sb_op_set(sb, RSLC_B_ADD,
            sb_swz(sb, gs_member(g, v, g->layout->tex0 + static_cast<int>(n)), c), e);
}
}  // Anonymous namespace

bool BuildGeometryShader(struct shader_build* sb, const void* arg)
{
  const GeometryShaderBuildArgs* a = static_cast<const GeometryShaderBuildArgs*>(arg);
  const ShaderHostConfig& host_config = *a->host_config;
  const geometry_shader_uid_data* uid_data = a->uid_data;
  const bool wireframe = host_config.wireframe;
  const bool stereo = host_config.stereo;
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];
  static const unsigned prims[4] = {RSLC_B_PRIM_POINTS, RSLC_B_PRIM_LINES,
                                    RSLC_B_PRIM_TRIANGLES, RSLC_B_PRIM_TRIANGLES};
  struct vs_output_layout plain, in_l, out_l;
  struct gs_build g;
  struct rslc_b_quals q;
  const struct rslc_type* gs_types[4];
  const char* gs_names[4];
  const struct rslc_type* vs_output;
  struct rslc_var *vs, *f, *offset = nullptr, *center = nullptr;
  struct rslc_func* fmain;
  uint32_t eye_init = 0, i_init = 0;

  if (wireframe)
    vertex_out++;

  memset(&g, 0, sizeof(g));
  g.sb = sb;
  g.a = a;
  g.wireframe = wireframe;
  g.stereo = stereo;

  // Insert layout parameters
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_IN;
  q.prim = prims[static_cast<u32>(primitive_type)];
  if (host_config.backend_gs_instancing)
    q.invocations = stereo ? 2 : 1;
  rslc_b_layout(sb->b, &q);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.prim = wireframe ? RSLC_B_PRIM_LINE_STRIP : RSLC_B_PRIM_TRIANGLE_STRIP;
  q.max_vertices =
      host_config.backend_gs_instancing ? vertex_out : (stereo ? vertex_out * 2 : vertex_out);
  rslc_b_layout(sb->b, &q);

  BuildLightStruct(sb);

  /* uniforms; Direct3D binds the stage's block at b0 */
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, a->api_type == APIType::D3D ? 1 : 4);
  BuildGeometryShaderUniforms(sb, gs_types, gs_names, 0);
  rslc_b_block_q(sb->b, &q, "GSBlock", nullptr, gs_types, gs_names, 4, g.gsblock);

  BuildVSOutputLayout(sb, uid_data->numTexGens, host_config, 0, &plain);
  vs_output = rslc_b_struct(sb->b, "VS_OUTPUT", plain.types, plain.names, plain.n);
  g.layout = &plain;

  BuildVSOutputLayout(sb, uid_data->numTexGens, host_config,
                      BuildInterpolationQuals(host_config.msaa, host_config.ssaa), &in_l);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_IN;
  q.location = sb_varying_location(sb, 0);
  vs = rslc_b_io_block(sb->b, &q, "VertexData", "vs", in_l.types, in_l.names, nullptr, in_l.quals,
                       in_l.n, static_cast<int32_t>(vertex_in));
  BuildVSOutputLayout(sb, uid_data->numTexGens, host_config,
                      BuildInterpolationQuals(host_config.msaa, host_config.ssaa), &out_l);
  q.storage = RSLC_B_ST_OUT;
  g.ps = rslc_b_io_block(sb->b, &q, "VertexData", "ps", out_l.types, out_l.names, nullptr,
                         out_l.quals, out_l.n, -1);
  if (stereo && !host_config.backend_gl_layer_in_fs)
  {
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_OUT;
    q.interp = RSLC_B_INTERP_FLAT;
    g.layer = rslc_b_declare(sb->b, &q, "layer", sb->i[1], 0);
  }

  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);

  if (primitive_type == PrimitiveType::Lines)
  {
    struct rslc_var* start = rslc_b_local(sb->b, "start", vs_output, 0, false);
    struct rslc_var* end = rslc_b_local(sb->b, "end", vs_output, 0, false);
    struct sb_place s_place = {start, nullptr, -1}, e_place = {end, nullptr, -1};
    struct sb_place v0 = {vs, nullptr, 0}, v1 = {vs, nullptr, 1};
    std::pair<struct rslc_var*, int> pa(end, plain.pos), pb(start, plain.pos);
    BuildAssignVSOutputMembers(sb, &plain, &s_place, &v0, host_config);
    BuildAssignVSOutputMembers(sb, &plain, &e_place, &v1, host_config);
    offset = BuildLineOffset(sb, g.gsblock[1], gs_pos_of, &pa, gs_pos_of, &pb, nullptr);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    struct sb_place c_place, v0 = {vs, nullptr, 0};
    center = rslc_b_local(sb->b, "center", vs_output, 0, false);
    c_place.var = center;
    c_place.index_var = nullptr;
    c_place.index = -1;
    BuildAssignVSOutputMembers(sb, &plain, &c_place, &v0, host_config);
    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
#define LP(c) sb_swz(sb, sb_ref(sb, g.gsblock[1]), c)
    offset = rslc_b_local(
        sb->b, "offset", sb->f[2],
        sb_bin(sb, RSLC_B_MUL,
               sb_con(sb, sb->f[2], 2, sb_bin(sb, RSLC_B_DIV, LP("w"), LP("x")),
                      sb_bin(sb, RSLC_B_DIV, sb_un(sb, RSLC_B_NEG, LP("w")), LP("y"))),
               sb_swz(sb, gs_member(&g, center, plain.pos), "w")),
        false);
#undef LP
  }

  if (stereo)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
    {
      g.eye = rslc_b_local(sb->b, "eye", sb->i[1], sb_bi(sb, "gl_InvocationID"), false);
    }
    else
    {
      eye_init = rslc_b_decl_stmt(sb->b, "eye", sb->i[1], sb_i(sb, 0), &g.eye);
      rslc_b_begin_block(sb->b);
    }
  }

  if (wireframe)
    g.first = rslc_b_local(sb->b, "first", vs_output, 0, false);

  // Avoid D3D warning about forced unrolling of single-iteration loop
  if (vertex_in > 1)
  {
    i_init = rslc_b_decl_stmt(sb->b, "i", sb->i[1], sb_i(sb, 0), &g.i);
    rslc_b_begin_block(sb->b);
  }
  else
  {
    g.i = rslc_b_local(sb->b, "i", sb->i[1], sb_i(sb, 0), false);
  }

  f = rslc_b_local(sb->b, "f", vs_output, 0, false);
  {
    struct sb_place f_place = {f, nullptr, -1}, vi = {vs, g.i, -1};
    BuildAssignVSOutputMembers(sb, &plain, &f_place, &vi, host_config);
  }

  if (host_config.backend_depth_clamp &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
  {
    // On certain GPUs we have to consume the clip distance from the vertex shader
    // or else the other vertex shader outputs will get corrupted.
    sb_set(sb, gs_member(&g, f, plain.clipDist0),
           sb_idx(sb, sb_mem(sb, sb_idx(sb, sb_bi(sb, "gl_in"), sb_ref(sb, g.i)), 2), sb_i(sb, 0)));
    sb_set(sb, gs_member(&g, f, plain.clipDist1),
           sb_idx(sb, sb_mem(sb, sb_idx(sb, sb_bi(sb, "gl_in"), sb_ref(sb, g.i)), 2), sb_i(sb, 1)));
  }

  if (stereo)
  {
    // For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
    // to the depth of the vertex. We retrieve the depth value from the w-component of the projected
    // vertex which contains the negated z-component of the original vertex.
    // For negative parallax (out-of-screen effects) we subtract a convergence value from
    // the depth value. This results in objects at a distance smaller than the convergence
    // distance to seemingly appear in front of the screen.
    // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
#define SP(c) sb_swz(sb, sb_ref(sb, g.gsblock[0]), c)
    struct rslc_var* hoffset = rslc_b_local(
        sb->b, "hoffset", sb->f[1],
        rslc_b_select(sb->b, sb_bin(sb, RSLC_B_EQ, sb_ref(sb, g.eye), sb_i(sb, 0)), SP("x"),
                      SP("y")),
        false);
    sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, gs_member(&g, f, plain.pos), "x"),
              sb_bin(sb, RSLC_B_MUL, sb_ref(sb, hoffset),
                     sb_bin(sb, RSLC_B_SUB, sb_swz(sb, gs_member(&g, f, plain.pos), "w"),
                            SP("z"))));
#undef SP
  }

#define TO(k) sb_idx(sb, sb_ref(sb, g.gsblock[2]), sb_i(sb, k))
  if (primitive_type == PrimitiveType::Lines)
  {
    struct rslc_var* l = rslc_b_local(sb->b, "l", vs_output, sb_ref(sb, f), false);
    struct rslc_var* r = rslc_b_local(sb->b, "r", vs_output, sb_ref(sb, f), false);
    uint32_t then_s;
    u32 i;
    sb_op_set(sb, RSLC_B_SUB, sb_swz(sb, gs_member(&g, l, plain.pos), "xy"),
              sb_bin(sb, RSLC_B_MUL, sb_ref(sb, offset),
                     sb_swz(sb, gs_member(&g, l, plain.pos), "w")));
    sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, gs_member(&g, r, plain.pos), "xy"),
              sb_bin(sb, RSLC_B_MUL, sb_ref(sb, offset),
                     sb_swz(sb, gs_member(&g, r, plain.pos), "w")));
    rslc_b_begin_block(sb->b);
    {
      struct rslc_var* tex_offset = rslc_b_local(
          sb->b, "texOffset", sb->f[1],
          sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0), sb_con(sb, sb->f[1], 1, TO(2))), false);
      for (i = 0; i < uid_data->numTexGens; ++i)
      {
        rslc_b_stmt(
            sb->b,
            rslc_b_if(sb->b, gs_tex_bit(&g, 0, i),
                      rslc_b_expr(sb->b,
                                  rslc_b_assign(sb->b, RSLC_B_ADD,
                                                sb_swz(sb,
                                                       gs_member(&g, r,
                                                                 plain.tex0 + static_cast<int>(i)),
                                                       "x"),
                                                sb_ref(sb, tex_offset))),
                      0));
      }
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, TO(2), sb_i(sb, 0)), then_s, 0));

    gs_emit_vertex(&g, l, true);
    gs_emit_vertex(&g, r, false);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    struct rslc_var* ll = rslc_b_local(sb->b, "ll", vs_output, sb_ref(sb, f), false);
    struct rslc_var* lr = rslc_b_local(sb->b, "lr", vs_output, sb_ref(sb, f), false);
    struct rslc_var* ul = rslc_b_local(sb->b, "ul", vs_output, sb_ref(sb, f), false);
    struct rslc_var* ur = rslc_b_local(sb->b, "ur", vs_output, sb_ref(sb, f), false);
    uint32_t then_s;
    u32 i;
#define NI(v) ((v) < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -(v))) : sb_i(sb, (v)))
#define CORNER(var, x, y)                                                                          \
  sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, gs_member(&g, var, plain.pos), "xy"),                      \
            sb_bin(sb, RSLC_B_MUL, sb_con(sb, sb->f[2], 2, NI(x), NI(y)), sb_ref(sb, offset)))
    CORNER(ll, -1, -1);
    CORNER(lr, 1, -1);
    CORNER(ul, -1, 1);
    sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, gs_member(&g, ur, plain.pos), "xy"), sb_ref(sb, offset));
    rslc_b_begin_block(sb->b);
    {
      struct rslc_var* tex_offset = rslc_b_local(
          sb->b, "texOffset", sb->f[2],
          sb_con(sb, sb->f[2], 2,
                 sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0), sb_con(sb, sb->f[1], 1, TO(3))),
                 sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0), sb_con(sb, sb->f[1], 1, TO(3)))),
          false);
      for (i = 0; i < uid_data->numTexGens; ++i)
      {
        uint32_t body;
        rslc_b_begin_block(sb->b);
        gs_tex_add(&g, ul, i, "xy",
                   sb_bin(sb, RSLC_B_MUL, sb_con(sb, sb->f[2], 2, sb_i(sb, 0), sb_i(sb, 1)),
                          sb_ref(sb, tex_offset)));
        gs_tex_add(&g, ur, i, "xy", sb_ref(sb, tex_offset));
        gs_tex_add(&g, lr, i, "xy",
                   sb_bin(sb, RSLC_B_MUL, sb_con(sb, sb->f[2], 2, sb_i(sb, 1), sb_i(sb, 0)),
                          sb_ref(sb, tex_offset)));
        body = rslc_b_end_block(sb->b);
        rslc_b_stmt(sb->b, rslc_b_if(sb->b, gs_tex_bit(&g, 1, i), body, 0));
      }
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, sb_bin(sb, RSLC_B_NE, TO(3), sb_i(sb, 0)), then_s, 0));
#undef CORNER
#undef NI
    gs_emit_vertex(&g, ll, true);
    gs_emit_vertex(&g, lr, false);
    gs_emit_vertex(&g, ul, false);
    gs_emit_vertex(&g, ur, false);
  }
  else
  {
    gs_emit_vertex(&g, f, true);
  }
#undef TO

  // Only close loop if previous code was in one (See D3D warning above)
  if (vertex_in > 1)
  {
    uint32_t body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_for(sb->b, i_init,
                                  sb_bin(sb, RSLC_B_LT, sb_ref(sb, g.i),
                                         sb_i(sb, static_cast<int32_t>(vertex_in))),
                                  sb_un(sb, RSLC_B_PREINC, sb_ref(sb, g.i)), body));
  }

  if (wireframe)
    gs_emit_vertex(&g, g.first, false);
  sb_do(sb, sb_call(sb, "EndPrimitive", 0));

  if (stereo && !host_config.backend_gs_instancing)
  {
    uint32_t body = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_for(sb->b, eye_init,
                                  sb_bin(sb, RSLC_B_LT, sb_ref(sb, g.eye), sb_i(sb, 2)),
                                  sb_un(sb, RSLC_B_PREINC, sb_ref(sb, g.eye)), body));
  }
  return rslc_b_func_end(sb->b);
}

u32 EnumerateGeometryShaderUids(GeometryShaderUid* uids)
{
  const PrimitiveType primitives[3] = {g_backend_info.bSupportsPrimitiveRestart ?
                                           PrimitiveType::TriangleStrip :
                                           PrimitiveType::Triangles,
                                       PrimitiveType::Lines, PrimitiveType::Points};
  u32 count = 0;
  u32 p, texgens;

  for (p = 0; p < 3; p++)
  {
    for (texgens = 0; texgens <= 8; texgens++)
    {
      geometry_shader_uid_data* const guid = uids[count].GetUidData();
      uids[count] = GeometryShaderUid();
      guid->primitive_type = static_cast<u32>(primitives[p]);
      guid->numTexGens = texgens;
      count++;
    }
  }
  return count;
}

