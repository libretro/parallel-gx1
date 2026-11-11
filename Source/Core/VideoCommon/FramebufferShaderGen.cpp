// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FramebufferShaderGen.h"

#include "VideoCommon/ShaderBuild.h"

#include "Common/Logging/Log.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace FramebufferShaderGen
{
namespace
{
APIType GetAPIType()
{
  return g_backend_info.api_type;
}
}  // Anonymous namespace

/* ---- stages built by calls ---- */

namespace
{
/* VertexData in or out: one unnamed block where the backend has
 * geometry shaders, a variable a member otherwise. */
struct fb_vary
{
  struct rslc_var* block;
  struct rslc_var* var[2];
};

uint32_t fb_vary_ref(const struct shader_build* sb, const struct fb_vary* v, u32 i)
{
  return v->block ? sb_mem(sb, sb_ref(sb, v->block), i) : sb_ref(sb, v->var[i]);
}

/* num_tex float3 v_texN, then num_colors float4 v_colN; a color is at
 * color_base + N where they are loose */
void fb_varyings(struct shader_build* sb, unsigned storage, u32 num_tex, u32 num_colors,
                 u32 color_base, struct fb_vary* v)
{
  static const char* const tex_names[2] = {"v_tex0", "v_tex1"};
  static const char* const col_names[2] = {"v_col0", "v_col1"};
  const struct rslc_type* types[4];
  const char* names[4];
  struct rslc_b_quals q;
  u32 n = 0, i;

  for (i = 0; i < num_tex; i++)
  {
    types[n] = sb->f[3];
    names[n++] = tex_names[i];
  }
  for (i = 0; i < num_colors; i++)
  {
    types[n] = sb->f[4];
    names[n++] = col_names[i];
  }
  v->block = NULL;
  rslc_b_quals_init(&q);
  q.storage = storage;
  if (g_backend_info.bSupportsGeometryShaders)
  {
    q.location = sb_varying_location(sb, 0);
    v->block = rslc_b_io_block(sb->b, &q, "VertexData", NULL, types, names, NULL, NULL, n, -1);
    return;
  }
  for (i = 0; i < n; i++)
  {
    q.location = sb_varying_location(sb, i < num_tex ? i : color_base + (i - num_tex));
    v->var[i] = rslc_b_declare(sb->b, &q, names[i], types[i], 0);
  }
}

struct fb_vertex
{
  struct rslc_var* rawcolor0;
  struct rslc_var* rawpos;
  struct fb_vary out;
};

/* a utility vertex stage's attributes, no texture ones, and its varyings */
void fb_vertex_decl(struct shader_build* sb, u32 num_color_inputs, bool position_input,
                    u32 num_tex_outputs, u32 num_color_outputs, struct fb_vertex* vx)
{
  struct rslc_b_quals q;
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_IN;
  vx->rawcolor0 = NULL;
  vx->rawpos = NULL;
  if (num_color_inputs)
  {
    q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::Color0));
    vx->rawcolor0 = rslc_b_declare(sb->b, &q, "rawcolor0", sb->f[4], 0);
  }
  if (position_input)
  {
    q.location = sb_attribute_location(sb, ShaderEnumValue(ShaderAttrib::Position));
    vx->rawpos = rslc_b_declare(sb->b, &q, "rawpos", sb->f[4], 0);
  }
  fb_varyings(sb, RSLC_B_ST_OUT, num_tex_outputs, num_color_outputs, 0, &vx->out);
}

/* PSBlock of float members, unnamed */
void fb_ps_block(struct shader_build* sb, const struct rslc_type* const* types,
                 const char* const* names, u32 n, struct rslc_var** members)
{
  struct rslc_b_quals q;
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 1);
  rslc_b_block_q(sb->b, &q, "PSBlock", NULL, types, names, n, members);
}

struct rslc_var* fb_sampler_n(struct shader_build* sb, u32 n, bool multisampled)
{
  static const char* const names[2] = {"samp0", "samp1"};
  struct rslc_b_quals q;
  rslc_b_quals_init(&q);
  sb_sampler(sb, &q, n);
  return rslc_b_declare(sb->b, &q, names[n],
                        rslc_b_sampler_type(sb->b,
                                            multisampled ? RSLC_B_SAMPLER_2D_MS_ARRAY :
                                                           RSLC_B_SAMPLER_2D_ARRAY,
                                            RSLC_BASE_FLOAT),
                        0);
}

struct rslc_var* fb_sampler(struct shader_build* sb, bool multisampled)
{
  return fb_sampler_n(sb, 0, multisampled);
}

/* a utility pixel stage's varyings and its output ocol0 */
struct rslc_var* fb_pixel_decl(struct shader_build* sb, u32 num_tex_inputs, u32 num_color_inputs,
                               const struct rslc_type* output_type, struct fb_vary* in)
{
  struct rslc_b_quals q;
  fb_varyings(sb, RSLC_B_ST_IN, num_tex_inputs, num_color_inputs, num_tex_inputs, in);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.location = sb_output_location(sb, 0);
  return rslc_b_declare(sb->b, &q, "ocol0", output_type, 0);
}

struct rslc_func* fb_main(struct shader_build* sb)
{
  struct rslc_func* f = rslc_b_func(sb->b, "main", sb->v, 0, NULL, NULL, NULL);
  rslc_b_func_begin(sb->b, f);
  return f;
}

/* float((id << 1) & 2), float(id & 2) */
void fb_quad_xy(const struct shader_build* sb, uint32_t* x, uint32_t* y)
{
  *x = sb_con(sb, sb->f[1], 1,
              sb_bin(sb, RSLC_B_BAND,
                     sb_bin(sb, RSLC_B_SHL, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, 1)),
                     sb_i(sb, 2)));
  *y = sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_BAND, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, 2)));
}

/* xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f) */
uint32_t fb_quad_ndc(const struct shader_build* sb, uint32_t xy)
{
  return sb_bin(sb, RSLC_B_ADD,
                sb_bin(sb, RSLC_B_MUL, xy,
                       sb_con(sb, sb->f[2], 2, sb_f(sb, 2.0),
                              sb_un(sb, RSLC_B_NEG, sb_f(sb, 2.0)))),
                sb_con(sb, sb->f[2], 2, sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0)), sb_f(sb, 1.0)));
}

/* opos.y = -opos.y; NDC space is flipped in Vulkan */
void fb_flip_y(const struct shader_build* sb)
{
  sb_set(sb, sb_swz(sb, sb_bi(sb, "gl_Position"), "y"),
         sb_un(sb, RSLC_B_NEG, sb_swz(sb, sb_bi(sb, "gl_Position"), "y")));
}

/* v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);
 * opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f); */
void fb_quad_tex(const struct shader_build* sb, const struct fb_vary* out)
{
  uint32_t x, y;
  fb_quad_xy(sb, &x, &y);
  sb_set(sb, fb_vary_ref(sb, out, 0), sb_con(sb, sb->f[3], 3, x, y, sb_f(sb, 0.0)));
  sb_set(sb, sb_bi(sb, "gl_Position"),
         sb_con(sb, sb->f[4], 3, fb_quad_ndc(sb, sb_swz(sb, fb_vary_ref(sb, out, 0), "xy")),
                sb_f(sb, 0.0), sb_f(sb, 1.0)));
}
}  // Anonymous namespace

bool BuildScreenQuadVertexShader(struct shader_build* sb, const void* /* arg */)
{
  struct fb_vertex vx;
  fb_vertex_decl(sb, 0, false, 1, 0, &vx);
  fb_main(sb);
  fb_quad_tex(sb, &vx.out);
  /* We also flip in GL so that (0,0) is in the lower-left. */
  if (GetAPIType() == APIType::Vulkan || GetAPIType() == APIType::OpenGL)
    fb_flip_y(sb);
  return rslc_b_func_end(sb->b);
}

bool BuildPassthroughGeometryShader(struct shader_build* sb, const void* arg)
{
  static const char* const tex_names[2] = {"v_tex0", "v_tex1"};
  static const char* const col_names[2] = {"v_col0", "v_col1"};
  const u32* const counts = static_cast<const u32*>(arg);
  const u32 num_tex = counts[0];
  const u32 num_colors = counts[1];
  const struct rslc_type* types[4];
  const char* names[4];
  struct rslc_var *v_in = NULL, *v_out = NULL, *j;
  struct rslc_b_quals q;
  uint32_t init, body;
  u32 n = 0, i, v;

  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_IN;
  q.prim = RSLC_B_PRIM_TRIANGLES;
  rslc_b_layout(sb->b, &q);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.prim = RSLC_B_PRIM_TRIANGLE_STRIP;
  q.max_vertices = 6;
  rslc_b_layout(sb->b, &q);
  for (i = 0; i < num_tex; i++)
  {
    types[n] = sb->f[3];
    names[n++] = tex_names[i];
  }
  for (i = 0; i < num_colors; i++)
  {
    types[n] = sb->f[4];
    names[n++] = col_names[i];
  }
  if (n)
  {
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.location = sb_varying_location(sb, 0);
    v_in = rslc_b_io_block(sb->b, &q, "VertexData", "v_in", types, names, NULL, NULL, n, 0);
    q.storage = RSLC_B_ST_OUT;
    v_out = rslc_b_io_block(sb->b, &q, "VertexData", "v_out", types, names, NULL, NULL, n, -1);
  }

  fb_main(sb);
  init = rslc_b_decl_stmt(sb->b, "j", sb->i[1], sb_i(sb, 0), &j);
  rslc_b_begin_block(sb->b);
  sb_set(sb, sb_bi(sb, "gl_Layer"), sb_ref(sb, j));
  // We have to explicitly unroll this loop otherwise the GL compiler gets cranky.
  for (v = 0; v < 3; v++)
  {
    // Outputs are undefined after EmitVertex()
    if (v > 0)
      sb_set(sb, sb_bi(sb, "gl_Layer"), sb_ref(sb, j));
    sb_set(sb, sb_bi(sb, "gl_Position"),
           sb_mem(sb, sb_idx(sb, sb_bi(sb, "gl_in"), sb_i(sb, static_cast<int32_t>(v))), 0));
    for (i = 0; i < n; i++)
    {
      const uint32_t src =
          sb_mem(sb, sb_idx(sb, sb_ref(sb, v_in), sb_i(sb, static_cast<int32_t>(v))), i);
      if (i < num_tex)
        sb_set(sb, sb_mem(sb, sb_ref(sb, v_out), i),
               sb_con(sb, sb->f[3], 2, sb_swz(sb, src, "xy"),
                      sb_con(sb, sb->f[1], 1, sb_ref(sb, j))));
      else
        sb_set(sb, sb_mem(sb, sb_ref(sb, v_out), i), src);
    }
    sb_do(sb, sb_call(sb, "EmitVertex", 0));
  }
  sb_do(sb, sb_call(sb, "EndPrimitive", 0));
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_for(sb->b, init, sb_bin(sb, RSLC_B_LT, sb_ref(sb, j), sb_i(sb, 2)),
                                sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, j)), body));
  return rslc_b_func_end(sb->b);
}

bool BuildTextureCopyVertexShader(struct shader_build* sb, const void* /* arg */)
{
  static const char* const names[2] = {"src_offset", "src_size"};
  const struct rslc_type* types[2];
  struct rslc_var* m[2];
  struct fb_vertex vx;
  types[0] = sb->f[2];
  types[1] = sb->f[2];
  fb_ps_block(sb, types, names, 2, m);
  fb_vertex_decl(sb, 0, false, 1, 0, &vx);
  fb_main(sb);
  fb_quad_tex(sb, &vx.out);
  sb_set(sb, fb_vary_ref(sb, &vx.out, 0),
         sb_con(sb, sb->f[3], 2,
                sb_bin(sb, RSLC_B_ADD, sb_ref(sb, m[0]),
                       sb_bin(sb, RSLC_B_MUL, sb_ref(sb, m[1]),
                              sb_swz(sb, fb_vary_ref(sb, &vx.out, 0), "xy"))),
                sb_f(sb, 0.0)));
  if (GetAPIType() == APIType::Vulkan || GetAPIType() == APIType::OpenGL)
    fb_flip_y(sb);
  return rslc_b_func_end(sb->b);
}

bool BuildTextureCopyPixelShader(struct shader_build* sb, const void* /* arg */)
{
  struct fb_vary in;
  struct rslc_var* samp = fb_sampler(sb, false);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[4], &in);
  fb_main(sb);
  sb_set(sb, sb_ref(sb, ocol0),
         sb_call(sb, "texture", 2, sb_ref(sb, samp), fb_vary_ref(sb, &in, 0)));
  return rslc_b_func_end(sb->b);
}

bool BuildColorPixelShader(struct shader_build* sb, const void* /* arg */)
{
  struct fb_vary in;
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 0, 1, sb->f[4], &in);
  fb_main(sb);
  sb_set(sb, sb_ref(sb, ocol0), fb_vary_ref(sb, &in, 0));
  return rslc_b_func_end(sb->b);
}

namespace
{
/* int layer = int(v_tex0.z); int3 coords = int3(int2(gl_FragCoord.xy), layer); */
struct rslc_var* fb_resolve_coords(struct shader_build* sb, const struct fb_vary* in)
{
  struct rslc_var* layer =
      rslc_b_local(sb->b, "layer", sb->i[1],
                   sb_con(sb, sb->i[1], 1, sb_swz(sb, fb_vary_ref(sb, in, 0), "z")), false);
  return rslc_b_local(
      sb->b, "coords", sb->i[3],
      sb_con(sb, sb->i[3], 2, sb_con(sb, sb->i[2], 1, sb_swz(sb, sb_bi(sb, "gl_FragCoord"), "xy")),
             sb_ref(sb, layer)),
      false);
}
}  // Anonymous namespace

bool BuildResolveColorPixelShader(struct shader_build* sb, const void* arg)
{
  const u32 samples = *static_cast<const u32*>(arg);
  struct fb_vary in;
  struct rslc_var* samp = fb_sampler(sb, true);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[4], &in);
  struct rslc_var *coords, *i;
  uint32_t init, body;
  fb_main(sb);
  coords = fb_resolve_coords(sb, &in);
  sb_set(sb, sb_ref(sb, ocol0), sb_con(sb, sb->f[4], 1, sb_f(sb, 0.0)));
  init = rslc_b_decl_stmt(sb->b, "i", sb->i[1], sb_i(sb, 0), &i);
  body = rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_ADD, sb_ref(sb, ocol0),
                                          sb_call(sb, "texelFetch", 3, sb_ref(sb, samp),
                                                  sb_ref(sb, coords), sb_ref(sb, i))));
  rslc_b_stmt(sb->b, rslc_b_for(sb->b, init,
                                sb_bin(sb, RSLC_B_LT, sb_ref(sb, i),
                                       sb_i(sb, static_cast<int32_t>(samples))),
                                sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, i)), body));
  sb_op_set(sb, RSLC_B_DIV, sb_ref(sb, ocol0), sb_f(sb, static_cast<double>(samples)));
  return rslc_b_func_end(sb->b);
}

bool BuildResolveDepthPixelShader(struct shader_build* sb, const void* arg)
{
  const u32 samples = *static_cast<const u32*>(arg);
  struct fb_vary in;
  struct rslc_var* samp = fb_sampler(sb, true);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[1], &in);
  struct rslc_var *coords, *i;
  uint32_t init, body;
  fb_main(sb);
  coords = fb_resolve_coords(sb, &in);
  // Take the minimum of all depth samples.
  sb_set(sb, sb_ref(sb, ocol0),
         sb_swz(sb,
                sb_call(sb, "texelFetch", 3, sb_ref(sb, samp), sb_ref(sb, coords), sb_i(sb, 0)),
                "r"));
  init = rslc_b_decl_stmt(sb->b, "i", sb->i[1], sb_i(sb, 1), &i);
  body = rslc_b_expr(
      sb->b, rslc_b_assign(sb->b, RSLC_B_SET, sb_ref(sb, ocol0),
                           sb_call(sb, "min", 2, sb_ref(sb, ocol0),
                                   sb_swz(sb,
                                          sb_call(sb, "texelFetch", 3, sb_ref(sb, samp),
                                                  sb_ref(sb, coords), sb_ref(sb, i)),
                                          "r"))));
  rslc_b_stmt(sb->b, rslc_b_for(sb->b, init,
                                sb_bin(sb, RSLC_B_LT, sb_ref(sb, i),
                                       sb_i(sb, static_cast<int32_t>(samples))),
                                sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, i)), body));
  return rslc_b_func_end(sb->b);
}

bool BuildClearVertexShader(struct shader_build* sb, const void* /* arg */)
{
  static const char* const names[2] = {"clear_color", "clear_depth"};
  const struct rslc_type* types[2];
  struct rslc_var* m[2];
  struct rslc_var* coord;
  struct fb_vertex vx;
  uint32_t x, y;
  types[0] = sb->f[4];
  types[1] = sb->f[1];
  fb_ps_block(sb, types, names, 2, m);
  fb_vertex_decl(sb, 0, false, 0, 1, &vx);
  fb_main(sb);
  fb_quad_xy(sb, &x, &y);
  coord = rslc_b_local(sb->b, "coord", sb->f[2], sb_con(sb, sb->f[2], 2, x, y), false);
  sb_set(sb, sb_bi(sb, "gl_Position"),
         sb_con(sb, sb->f[4], 3, fb_quad_ndc(sb, sb_ref(sb, coord)), sb_ref(sb, m[1]),
                sb_f(sb, 1.0)));
  sb_set(sb, fb_vary_ref(sb, &vx.out, 0), sb_ref(sb, m[0]));
  if (GetAPIType() == APIType::Vulkan)
    fb_flip_y(sb);
  return rslc_b_func_end(sb->b);
}

bool BuildEFBPokeVertexShader(struct shader_build* sb, const void* /* arg */)
{
  struct fb_vertex vx;
  fb_vertex_decl(sb, 1, true, 0, 1, &vx);
  fb_main(sb);
  sb_set(sb, fb_vary_ref(sb, &vx.out, 0), sb_ref(sb, vx.rawcolor0));
  sb_set(sb, sb_bi(sb, "gl_Position"),
         sb_con(sb, sb->f[4], 2, sb_swz(sb, sb_ref(sb, vx.rawpos), "xyz"), sb_f(sb, 1.0)));
  if (g_backend_info.bSupportsLargePoints)
    sb_set(sb, sb_bi(sb, "gl_PointSize"), sb_swz(sb, sb_ref(sb, vx.rawpos), "w"));
  if (GetAPIType() == APIType::Vulkan)
    fb_flip_y(sb);
  return rslc_b_func_end(sb->b);
}

namespace
{
/* round(val * scale) as int4 */
uint32_t fb_scaled_int4(const struct shader_build* sb, struct rslc_var* val, double scale)
{
  return sb_con(sb, sb->i[4], 1,
                sb_call(sb, "round", 1, sb_bin(sb, RSLC_B_MUL, sb_ref(sb, val), sb_f(sb, scale))));
}

/* dst.c = e; */
void fb_set_c(const struct shader_build* sb, struct rslc_var* v, const char* c, uint32_t e)
{
  sb_set(sb, sb_swz(sb, sb_ref(sb, v), c), e);
}

uint32_t fb_c(const struct shader_build* sb, struct rslc_var* v, const char* c)
{
  return sb_swz(sb, sb_ref(sb, v), c);
}

uint32_t fb_and(const struct shader_build* sb, uint32_t e, int32_t m)
{
  return sb_bin(sb, RSLC_B_BAND, e, sb_i(sb, m));
}

uint32_t fb_shl(const struct shader_build* sb, uint32_t e, int32_t n)
{
  return sb_bin(sb, RSLC_B_SHL, e, sb_i(sb, n));
}

uint32_t fb_shr(const struct shader_build* sb, uint32_t e, int32_t n)
{
  return sb_bin(sb, RSLC_B_SHR, e, sb_i(sb, n));
}

uint32_t fb_or(const struct shader_build* sb, uint32_t a, uint32_t b)
{
  return sb_bin(sb, RSLC_B_BOR, a, b);
}
}  // Anonymous namespace

bool BuildFormatConversionShader(struct shader_build* sb, const void* arg)
{
  const struct FormatConversionArgs* a = static_cast<const struct FormatConversionArgs*>(arg);
  struct fb_vary in;
  struct rslc_var* samp = fb_sampler(sb, a->samples > 1);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[4], &in);
  struct rslc_var *coords, *val, *src, *dst;
  fb_main(sb);
  coords = fb_resolve_coords(sb, &in);

  if (a->samples == 1)
  {
    // No MSAA at all.
    val = rslc_b_local(sb->b, "val", sb->f[4],
                       sb_call(sb, "texelFetch", 3, sb_ref(sb, samp), sb_ref(sb, coords),
                               sb_i(sb, 0)),
                       false);
  }
  else if (g_ActiveConfig.bSSAA)
  {
    // Sample shading, shader runs once per sample
    val = rslc_b_local(sb->b, "val", sb->f[4],
                       sb_call(sb, "texelFetch", 3, sb_ref(sb, samp), sb_ref(sb, coords),
                               sb_bi(sb, "gl_SampleID")),
                       false);
  }
  else
  {
    // MSAA without sample shading, average out all samples.
    struct rslc_var* i;
    uint32_t init, body;
    val = rslc_b_local(sb->b, "val", sb->f[4],
                       sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0),
                              sb_f(sb, 0.0)),
                       false);
    init = rslc_b_decl_stmt(sb->b, "i", sb->i[1], sb_i(sb, 0), &i);
    body = rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_ADD, sb_ref(sb, val),
                                            sb_call(sb, "texelFetch", 3, sb_ref(sb, samp),
                                                    sb_ref(sb, coords), sb_ref(sb, i))));
    rslc_b_stmt(sb->b, rslc_b_for(sb->b, init,
                                  sb_bin(sb, RSLC_B_LT, sb_ref(sb, i),
                                         sb_i(sb, static_cast<int32_t>(a->samples))),
                                  sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, i)), body));
    sb_op_set(sb, RSLC_B_DIV, sb_ref(sb, val),
              sb_con(sb, sb->f[1], 1, sb_i(sb, static_cast<int32_t>(a->samples))));
  }

  switch (a->convtype)
  {
  case EFBReinterpretType::RGB8ToRGBA6:
    src = rslc_b_local(sb->b, "src8", sb->i[4], fb_scaled_int4(sb, val, 255.0), false);
    dst = rslc_b_local(sb->b, "dst6", sb->i[4], 0, false);
    fb_set_c(sb, dst, "r", fb_shr(sb, fb_c(sb, src, "r"), 2));
    fb_set_c(sb, dst, "g",
             fb_or(sb, fb_shl(sb, fb_and(sb, fb_c(sb, src, "r"), 0x3), 4),
                   fb_shr(sb, fb_c(sb, src, "g"), 4)));
    fb_set_c(sb, dst, "b",
             fb_or(sb, fb_shl(sb, fb_and(sb, fb_c(sb, src, "g"), 0xF), 2),
                   fb_shr(sb, fb_c(sb, src, "b"), 6)));
    fb_set_c(sb, dst, "a", fb_and(sb, fb_c(sb, src, "b"), 0x3F));
    sb_set(sb, sb_ref(sb, ocol0),
           sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, sb_ref(sb, dst)), sb_f(sb, 63.0)));
    break;

  case EFBReinterpretType::RGBA6ToRGB8:
    src = rslc_b_local(sb->b, "src6", sb->i[4], fb_scaled_int4(sb, val, 63.0), false);
    dst = rslc_b_local(sb->b, "dst8", sb->i[4], 0, false);
    fb_set_c(sb, dst, "r",
             fb_or(sb, fb_shl(sb, fb_c(sb, src, "r"), 2), fb_shr(sb, fb_c(sb, src, "g"), 4)));
    fb_set_c(sb, dst, "g",
             fb_or(sb, fb_shl(sb, fb_and(sb, fb_c(sb, src, "g"), 0xF), 4),
                   fb_shr(sb, fb_c(sb, src, "b"), 2)));
    fb_set_c(sb, dst, "b",
             fb_or(sb, fb_shl(sb, fb_and(sb, fb_c(sb, src, "b"), 0x3), 6), fb_c(sb, src, "a")));
    fb_set_c(sb, dst, "a", sb_i(sb, 255));
    sb_set(sb, sb_ref(sb, ocol0),
           sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, sb_ref(sb, dst)), sb_f(sb, 255.0)));
    break;

  default:
    sb_set(sb, sb_ref(sb, ocol0), sb_ref(sb, val));
    break;
  }
  return rslc_b_func_end(sb->b);
}

namespace
{
/* uint(t.c * scale) */
uint32_t fb_uq(const struct shader_build* sb, struct rslc_var* t, const char* c, double scale)
{
  return sb_con(sb, sb->u[1], 1, sb_bin(sb, RSLC_B_MUL, fb_c(sb, t, c), sb_f(sb, scale)));
}

/* float((raw >> shift) & mask) / div; no shift for 0 */
uint32_t fb_field(const struct shader_build* sb, struct rslc_var* raw, int32_t shift, uint32_t mask,
                  double div)
{
  uint32_t e = sb_ref(sb, raw);
  if (shift)
    e = fb_shr(sb, e, shift);
  return sb_bin(sb, RSLC_B_DIV,
                sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_BAND, e, sb_u(sb, mask))), sb_f(sb, div));
}
}  // Anonymous namespace

bool BuildTextureReinterpretShader(struct shader_build* sb, const void* arg)
{
  const TextureFormat* const formats = static_cast<const TextureFormat*>(arg);
  const TextureFormat from_format = formats[0];
  const TextureFormat to_format = formats[1];
  struct fb_vary in;
  struct rslc_var* samp = fb_sampler(sb, false);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[4], &in);
  struct rslc_var *layer, *coords, *raw, *t, *o;
  uint32_t then_s, else_s;

  switch (from_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
  case TextureFormat::IA8:
  case TextureFormat::I4:
  case TextureFormat::IA4:
  case TextureFormat::RGB565:
  case TextureFormat::RGB5A3:
    break;
  default:
    WARN_LOG(VIDEO, "From format %u is not supported", static_cast<unsigned>(from_format));
    return false;
  }
  switch (to_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
  case TextureFormat::IA8:
  case TextureFormat::IA4:
  case TextureFormat::RGB565:
  case TextureFormat::RGB5A3:
    break;
  default:
    WARN_LOG(VIDEO, "To format %u is not supported", static_cast<unsigned>(to_format));
    return false;
  }

  fb_main(sb);
  layer = rslc_b_local(sb->b, "layer", sb->i[1],
                       sb_con(sb, sb->i[1], 1, sb_swz(sb, fb_vary_ref(sb, &in, 0), "z")), false);
  coords = rslc_b_local(
      sb->b, "coords", sb->i[4],
      sb_con(sb, sb->i[4], 3, sb_con(sb, sb->i[2], 1, sb_swz(sb, sb_bi(sb, "gl_FragCoord"), "xy")),
             sb_ref(sb, layer), sb_i(sb, 0)),
      false);

  // Convert to a 32-bit value encompassing all channels, filling the most significant bits with
  // zeroes.
  raw = rslc_b_local(sb->b, "raw_value", sb->u[1], 0, false);
  t = rslc_b_local(sb->b, "temp_value", sb->f[4],
                   sb_call(sb, "texelFetch", 3, sb_ref(sb, samp),
                           sb_swz(sb, sb_ref(sb, coords), "xyz"), sb_swz(sb, sb_ref(sb, coords), "w")),
                   false);
  switch (from_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
    sb_set(sb, sb_ref(sb, raw), fb_uq(sb, t, "r", 255.0));
    break;
  case TextureFormat::IA8:
    sb_set(sb, sb_ref(sb, raw),
           fb_or(sb, fb_uq(sb, t, "r", 255.0), fb_shl(sb, fb_uq(sb, t, "a", 255.0), 8)));
    break;
  case TextureFormat::I4:
    sb_set(sb, sb_ref(sb, raw), fb_uq(sb, t, "r", 15.0));
    break;
  case TextureFormat::IA4:
    sb_set(sb, sb_ref(sb, raw),
           fb_or(sb, fb_uq(sb, t, "r", 15.0), fb_shl(sb, fb_uq(sb, t, "a", 15.0), 4)));
    break;
  case TextureFormat::RGB565:
    sb_set(sb, sb_ref(sb, raw),
           fb_or(sb,
                 fb_or(sb, fb_uq(sb, t, "b", 31.0), fb_shl(sb, fb_uq(sb, t, "g", 63.0), 5)),
                 fb_shl(sb, fb_uq(sb, t, "r", 31.0), 11)));
    break;
  default:  // RGB5A3
    // 0.8784 = 224 / 255 which is the maximum alpha value that can be represented in 3 bits
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, raw),
           fb_or(sb,
                 fb_or(sb,
                       fb_or(sb, fb_uq(sb, t, "b", 31.0), fb_shl(sb, fb_uq(sb, t, "g", 31.0), 5)),
                       fb_shl(sb, fb_uq(sb, t, "r", 31.0), 10)),
                 sb_u(sb, 0x8000u)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, raw),
           fb_or(sb,
                 fb_or(sb,
                       fb_or(sb, fb_uq(sb, t, "b", 15.0), fb_shl(sb, fb_uq(sb, t, "g", 15.0), 4)),
                       fb_shl(sb, fb_uq(sb, t, "r", 15.0), 8)),
                 fb_shl(sb, fb_uq(sb, t, "a", 7.0), 12)));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                                 sb_bin(sb, RSLC_B_GT, fb_c(sb, t, "a"), sb_f(sb, 0.878)),
                                 then_s, else_s));
    break;
  }

  // Now convert it to its new representation.
  switch (to_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
    o = rslc_b_local(sb->b, "orgba", sb->f[1], fb_field(sb, raw, 0, 0xFFu, 255.0), false);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, sb_ref(sb, o), sb_ref(sb, o), sb_ref(sb, o), sb_ref(sb, o)));
    break;
  case TextureFormat::IA8:
    o = rslc_b_local(sb->b, "orgb", sb->f[1], fb_field(sb, raw, 0, 0xFFu, 255.0), false);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, sb_ref(sb, o), sb_ref(sb, o), sb_ref(sb, o),
                  fb_field(sb, raw, 8, 0xFFu, 255.0)));
    break;
  case TextureFormat::IA4:
    o = rslc_b_local(sb->b, "orgb", sb->f[1], fb_field(sb, raw, 0, 0xFu, 15.0), false);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, sb_ref(sb, o), sb_ref(sb, o), sb_ref(sb, o),
                  fb_field(sb, raw, 4, 0xFu, 15.0)));
    break;
  case TextureFormat::RGB565:
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, fb_field(sb, raw, 10, 0x1Fu, 31.0),
                  fb_field(sb, raw, 5, 0x1Fu, 31.0), fb_field(sb, raw, 0, 0x1Fu, 31.0),
                  sb_f(sb, 1.0)));
    break;
  default:  // RGB5A3
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, fb_field(sb, raw, 10, 0x1Fu, 31.0),
                  fb_field(sb, raw, 5, 0x1Fu, 31.0), fb_field(sb, raw, 0, 0x1Fu, 31.0),
                  sb_f(sb, 1.0)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, fb_field(sb, raw, 8, 0x0Fu, 15.0),
                  fb_field(sb, raw, 4, 0x0Fu, 15.0), fb_field(sb, raw, 0, 0x0Fu, 15.0),
                  fb_field(sb, raw, 12, 0x07u, 7.0)));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b,
                          sb_bin(sb, RSLC_B_NE,
                                 sb_bin(sb, RSLC_B_BAND, sb_ref(sb, raw), sb_u(sb, 0x8000u)),
                                 sb_u(sb, 0u)),
                          then_s, else_s));
    break;
  }
  return rslc_b_func_end(sb->b);
}

bool BuildEFBRestorePixelShader(struct shader_build* sb, const void* /* arg */)
{
  struct fb_vary in;
  struct rslc_var* samp0 = fb_sampler_n(sb, 0, false);
  struct rslc_var* samp1 = fb_sampler_n(sb, 1, false);
  struct rslc_var* ocol0 = fb_pixel_decl(sb, 1, 0, sb->f[4], &in);
  fb_main(sb);
  sb_set(sb, sb_ref(sb, ocol0),
         sb_call(sb, "texture", 2, sb_ref(sb, samp0), fb_vary_ref(sb, &in, 0)));
  sb_set(sb, sb_bi(sb, "gl_FragDepth"),
         sb_swz(sb, sb_call(sb, "texture", 2, sb_ref(sb, samp1), fb_vary_ref(sb, &in, 0)), "r"));
  return rslc_b_func_end(sb->b);
}

}  // namespace FramebufferShaderGen

