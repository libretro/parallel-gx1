// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureConverterShaderGen.h"

#include <cstring>

#include "VideoCommon/ShaderBuild.h"

#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace TextureConversionShaderGen
{
static_assert(COPY_FLAG_ALL_FILTER_COEFS == 1 && COPY_FLAG_EFB_HAS_ALPHA == 2 &&
                  COPY_FLAG_FILTER_CAN_OVERFLOW == 4 && COPY_FLAG_APPLY_GAMMA == 8 &&
                  COPY_FLAG_DEPTH == 16 && COPY_FLAG_INTENSITY == 32,
              "the pixel shader spells them out");

/* A format's channels, in copy_flags: for each of R, G, B and A, the component of the copied
 * colour it takes (2 bits each from bit 8: R, G, B, A), the bits of it the format keeps (3 bits
 * each from bit 16: COPY_MASK_*) and whether it is 1 instead (a bit each from bit 28). */
enum
{
  COPY_MASK_FF = 0,
  COPY_MASK_F0,
  COPY_MASK_F8,
  COPY_MASK_FC,
  COPY_MASK_E0
};

static u32 FormatLayout(u32 r, u32 g, u32 b, u32 a, u32 mr, u32 mg, u32 mb, u32 ma, u32 one)
{
  return (r << 8) | (g << 10) | (b << 12) | (a << 14) | (mr << 16) | (mg << 19) | (mb << 22) |
         (ma << 25) | (one << 28);
}

static u32 GetFormatLayout(EFBCopyFormat dst_format)
{
  enum
  {
    R = 0,
    G,
    B,
    A
  };
  switch (dst_format)
  {
  case EFBCopyFormat::R4:  // R4: float(r & 0xF0) / 240, in all four
    return FormatLayout(R, R, R, R, COPY_MASK_F0, COPY_MASK_F0, COPY_MASK_F0, COPY_MASK_F0, 0);
  case EFBCopyFormat::R8_0x1:  // R8
  case EFBCopyFormat::R8:      // R8
    return FormatLayout(R, R, R, R, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::RA4:  // RA4: (r, a) & 0xF0 / 240, as rrra
    return FormatLayout(R, R, R, A, COPY_MASK_F0, COPY_MASK_F0, COPY_MASK_F0, COPY_MASK_F0, 0);
  case EFBCopyFormat::RA8:
    return FormatLayout(R, R, R, A, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::A8:
    return FormatLayout(A, A, A, A, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::G8:
    return FormatLayout(G, G, G, G, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::B8:
    return FormatLayout(B, B, B, B, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::RG8:
    return FormatLayout(R, R, R, G, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::GB8:
    return FormatLayout(G, G, G, B, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  case EFBCopyFormat::RGB565:  // r and b & 0xF8 / 248, g & 0xFC / 252, alpha 1
    return FormatLayout(R, G, B, A, COPY_MASK_F8, COPY_MASK_FC, COPY_MASK_F8, COPY_MASK_FF, 8);
  case EFBCopyFormat::RGB5A3:
    // TODO: The MSB controls whether we have RGB5 or RGB4A3, this selection
    // will need to be implemented once we move away from floats.
    return FormatLayout(R, G, B, A, COPY_MASK_F8, COPY_MASK_F8, COPY_MASK_F8, COPY_MASK_E0, 0);
  case EFBCopyFormat::XFB:  // rgb / 255, alpha 1
    return FormatLayout(R, G, B, A, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 8);
  case EFBCopyFormat::RGBA8:
  default:
    return FormatLayout(R, G, B, A, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, COPY_MASK_FF, 0);
  }
}

EFBCopyFormat GetDrawnFormat(EFBCopyFormat dst_format)
{
  if (!g_ActiveConfig.bForceTrueColor)
    return dst_format;
  // Increase the precision of EFB copies where it's likely to be safe.
  switch (dst_format)
  {
  case EFBCopyFormat::RGB565:
    // HACK: XFB is RGB8.
    // Don't blindly do this in other places though,
    // the enum value is used to identify XFB copies.
    // The important thing here is that we need alpha = 1.
    return EFBCopyFormat::XFB;
  case EFBCopyFormat::RGB5A3:
    return EFBCopyFormat::RGBA8;
  default:
    // Let's not touch the other formats for now, seems risky.
    return dst_format;
  }
}

u32 GetCopyFlags(EFBCopyFormat dst_format, bool is_depth_copy, bool is_intensity, float gamma_rcp,
                 const std::array<u32, 3>& filter_coefficients)
{
  return (TextureCacheBase::AllCopyFilterCoefsNeeded(filter_coefficients) ?
              COPY_FLAG_ALL_FILTER_COEFS :
              0u) |
         (bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24 ? COPY_FLAG_EFB_HAS_ALPHA : 0u) |
         (TextureCacheBase::CopyFilterCanOverflow(filter_coefficients) ?
              COPY_FLAG_FILTER_CAN_OVERFLOW :
              0u) |
         // If the gamma is needed, then include that too.
         (gamma_rcp != 1.0f ? COPY_FLAG_APPLY_GAMMA : 0u) |
         (is_depth_copy ? COPY_FLAG_DEPTH : 0u) | (is_intensity ? COPY_FLAG_INTENSITY : 0u) |
         GetFormatLayout(GetDrawnFormat(dst_format));
}

/* ---- stages built by calls ---- */

namespace
{
struct copy_block
{
  struct rslc_var* m[7];  // src_offset, src_size, filter_coefficients, gamma_rcp, clamp_tb,
                          // pixel_height, copy_flags
};

void copy_ps_block(struct shader_build* sb, struct copy_block* blk)
{
  static const char* const names[7] = {"src_offset", "src_size", "filter_coefficients",
                                       "gamma_rcp",  "clamp_tb", "pixel_height",
                                       "copy_flags"};
  const struct rslc_type* types[7];
  struct rslc_b_quals q;
  types[0] = sb->f[2];
  types[1] = sb->f[2];
  types[2] = sb->u[3];
  types[3] = sb->f[1];
  types[4] = sb->f[2];
  types[5] = sb->f[1];
  types[6] = sb->u[1];
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 1);
  rslc_b_block_q(sb->b, &q, "PSBlock", NULL, types, names, 7, blk->m);
}

/* v_tex0, in or out: in VertexData where there are geometry shaders */
uint32_t copy_v_tex0(struct shader_build* sb, unsigned storage, struct rslc_var** v)
{
  static const char* const name = "v_tex0";
  struct rslc_b_quals q;
  rslc_b_quals_init(&q);
  q.storage = storage;
  q.location = sb_varying_location(sb, 0);
  if (g_backend_info.bSupportsGeometryShaders)
  {
    *v = rslc_b_io_block(sb->b, &q, "VertexData", NULL, &sb->f[3], &name, NULL, NULL, 1, -1);
    return 1;
  }
  *v = rslc_b_declare(sb->b, &q, name, sb->f[3], 0);
  return 0;
}

uint32_t copy_tex0(const struct shader_build* sb, struct rslc_var* v, uint32_t in_block)
{
  return in_block ? sb_mem(sb, sb_ref(sb, v), 0) : sb_ref(sb, v);
}

/* (copy_flags & bit) != 0u */
uint32_t copy_flag(const struct shader_build* sb, const struct copy_block* blk, uint32_t bit)
{
  return sb_bin(sb, RSLC_B_NE, sb_bin(sb, RSLC_B_BAND, sb_ref(sb, blk->m[6]), sb_u(sb, bit)),
                sb_u(sb, 0u));
}

uint32_t copy_raw(const struct shader_build* sb, struct rslc_var* raw, const char* c)
{
  return sb_swz(sb, sb_ref(sb, raw), c);
}

/* float<n>(texcol_raw.c & mask) / div */
uint32_t copy_masked(const struct shader_build* sb, struct rslc_var* raw, const char* c,
                     uint32_t mask, double div)
{
  const unsigned n = static_cast<unsigned>(strlen(c));
  return sb_bin(sb, RSLC_B_DIV,
                sb_con(sb, sb->f[n], 1, sb_bin(sb, RSLC_B_BAND, copy_raw(sb, raw, c), sb_u(sb, mask))),
                sb_f(sb, div));
}

/* The output of one format, as a shader built for it alone writes it. */
void copy_format_body(struct shader_build* sb, struct rslc_var* raw, struct rslc_var* ocol0,
                      EFBCopyFormat format)
{
  static const char* const swz[] = {"rrrr", "rrra", "aaaa", "gggg", "bbbb", "rrrg", "gggb"};
  struct rslc_var *a, *b;
  const char* splat = nullptr;
  switch (format)
  {
  case EFBCopyFormat::R4:
    a = rslc_b_local(sb->b, "red", sb->f[1], copy_masked(sb, raw, "r", 0xF0u, 240.0), false);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, sb_ref(sb, a), sb_ref(sb, a), sb_ref(sb, a), sb_ref(sb, a)));
    return;
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
    splat = swz[0];
    break;
  case EFBCopyFormat::RA4:
    a = rslc_b_local(sb->b, "red_alpha", sb->f[2], copy_masked(sb, raw, "ra", 0xF0u, 240.0),
                     false);
    sb_set(sb, sb_ref(sb, ocol0), sb_swz(sb, sb_ref(sb, a), "rrrg"));
    return;
  case EFBCopyFormat::RA8:
    splat = swz[1];
    break;
  case EFBCopyFormat::A8:
    splat = swz[2];
    break;
  case EFBCopyFormat::G8:
    splat = swz[3];
    break;
  case EFBCopyFormat::B8:
    splat = swz[4];
    break;
  case EFBCopyFormat::RG8:
    splat = swz[5];
    break;
  case EFBCopyFormat::GB8:
    splat = swz[6];
    break;
  case EFBCopyFormat::RGB565:
    a = rslc_b_local(sb->b, "red_blue", sb->f[2], copy_masked(sb, raw, "rb", 0xF8u, 248.0),
                     false);
    b = rslc_b_local(sb->b, "green", sb->f[1], copy_masked(sb, raw, "g", 0xFCu, 252.0), false);
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 4, sb_swz(sb, sb_ref(sb, a), "r"), sb_ref(sb, b),
                  sb_swz(sb, sb_ref(sb, a), "g"), sb_f(sb, 1.0)));
    return;
  case EFBCopyFormat::RGB5A3:
    // TODO: The MSB controls whether we have RGB5 or RGB4A3, this selection
    // will need to be implemented once we move away from floats.
    a = rslc_b_local(sb->b, "color", sb->f[3], copy_masked(sb, raw, "rgb", 0xF8u, 248.0), false);
    b = rslc_b_local(sb->b, "alpha", sb->f[1], copy_masked(sb, raw, "a", 0xE0u, 224.0), false);
    sb_set(sb, sb_ref(sb, ocol0), sb_con(sb, sb->f[4], 2, sb_ref(sb, a), sb_ref(sb, b)));
    return;
  case EFBCopyFormat::XFB:
    sb_set(sb, sb_ref(sb, ocol0),
           sb_con(sb, sb->f[4], 2,
                  sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[3], 1, copy_raw(sb, raw, "rgb")),
                         sb_f(sb, 255.0)),
                  sb_f(sb, 1.0)));
    return;
  case EFBCopyFormat::RGBA8:
  default:
    sb_set(sb, sb_ref(sb, ocol0),
           sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, copy_raw(sb, raw, "rgba")),
                  sb_f(sb, 255.0)));
    return;
  }
  sb_set(sb, sb_ref(sb, ocol0),
         sb_bin(sb, RSLC_B_DIV, sb_swz(sb, sb_con(sb, sb->f[4], 1, sb_ref(sb, raw)), splat),
                sb_f(sb, 255.0)));
}

/* c == 0u ? r : c == 1u ? g : c == 2u ? b : a */
uint32_t copy_pick(const struct shader_build* sb, struct rslc_var* source, const char* c,
                   struct rslc_var* raw)
{
  uint32_t e = copy_raw(sb, raw, "a");
  static const char* const ch[3] = {"r", "g", "b"};
  int k;
  for (k = 2; k >= 0; k--)
    e = rslc_b_select(sb->b,
                      sb_bin(sb, RSLC_B_EQ, sb_swz(sb, sb_ref(sb, source), c),
                             sb_u(sb, static_cast<uint32_t>(k))),
                      copy_raw(sb, raw, ch[k]), e);
  return e;
}

/* c == 1u ? 0xF0u : c == 2u ? 0xF8u : c == 3u ? 0xFCu : c == 4u ? 0xE0u : 0xFFu */
uint32_t copy_mask_of(const struct shader_build* sb, struct rslc_var* mask, const char* c)
{
  static const uint32_t masks[4] = {0xF0u, 0xF8u, 0xFCu, 0xE0u};
  uint32_t e = sb_u(sb, 0xFFu);
  int k;
  for (k = 3; k >= 0; k--)
    e = rslc_b_select(sb->b,
                      sb_bin(sb, RSLC_B_EQ, sb_swz(sb, sb_ref(sb, mask), c),
                             sb_u(sb, static_cast<uint32_t>(k + 1))),
                      sb_u(sb, masks[k]), e);
  return e;
}

/* (uint4(copy_flags) >> uint4(a, b, c, d)) & m */
uint32_t copy_fields(const struct shader_build* sb, const struct copy_block* blk, uint32_t a,
                     uint32_t b, uint32_t c, uint32_t d, uint32_t m)
{
  return sb_bin(sb, RSLC_B_BAND,
                sb_bin(sb, RSLC_B_SHR, sb_con(sb, sb->u[4], 1, sb_ref(sb, blk->m[6])),
                       sb_con(sb, sb->u[4], 4, sb_u(sb, a), sb_u(sb, b), sb_u(sb, c), sb_u(sb, d))),
                sb_u(sb, m));
}
}  // Anonymous namespace

bool BuildVertexShader(struct shader_build* sb, const void* /* arg */)
{
  struct copy_block blk;
  struct rslc_var* v;
  struct rslc_func* f;
  uint32_t in_block, x, y;
  copy_ps_block(sb, &blk);
  in_block = copy_v_tex0(sb, RSLC_B_ST_OUT, &v);
  f = rslc_b_func(sb->b, "main", sb->v, 0, NULL, NULL, NULL);
  rslc_b_func_begin(sb->b, f);
  x = sb_con(sb, sb->f[1], 1,
             sb_bin(sb, RSLC_B_BAND,
                    sb_bin(sb, RSLC_B_SHL, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, 1)), sb_i(sb, 2)));
  y = sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_BAND, sb_bi(sb, "gl_VertexIndex"), sb_i(sb, 2)));
  sb_set(sb, copy_tex0(sb, v, in_block), sb_con(sb, sb->f[3], 3, x, y, sb_f(sb, 0.0)));
  sb_set(sb, sb_bi(sb, "gl_Position"),
         sb_con(sb, sb->f[4], 3,
                sb_bin(sb, RSLC_B_ADD,
                       sb_bin(sb, RSLC_B_MUL, sb_swz(sb, copy_tex0(sb, v, in_block), "xy"),
                              sb_con(sb, sb->f[2], 2, sb_f(sb, 2.0),
                                     sb_un(sb, RSLC_B_NEG, sb_f(sb, 2.0)))),
                       sb_con(sb, sb->f[2], 2, sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0)),
                              sb_f(sb, 1.0))),
                sb_f(sb, 0.0), sb_f(sb, 1.0)));
  sb_set(sb, copy_tex0(sb, v, in_block),
         sb_con(sb, sb->f[3], 2,
                sb_bin(sb, RSLC_B_ADD, sb_ref(sb, blk.m[0]),
                       sb_bin(sb, RSLC_B_MUL, sb_ref(sb, blk.m[1]),
                              sb_swz(sb, copy_tex0(sb, v, in_block), "xy"))),
                sb_f(sb, 0.0)));
  // NDC space is flipped in Vulkan
  if (g_backend_info.api_type == APIType::Vulkan)
    sb_set(sb, sb_swz(sb, sb_bi(sb, "gl_Position"), "y"),
           sb_un(sb, RSLC_B_NEG, sb_swz(sb, sb_bi(sb, "gl_Position"), "y")));
  return rslc_b_func_end(sb->b);
}

bool BuildPixelShader(struct shader_build* sb, const void* arg)
{
  static const unsigned in_dirs[2] = {RSLC_B_IN, RSLC_B_IN};
  static const char* const in_names[2] = {"uv", "y_offset"};
  const EFBCopyFormat* const format = static_cast<const EFBCopyFormat*>(arg);
  const struct rslc_type* in_types[2];
  struct copy_block blk;
  struct rslc_b_quals q;
  struct rslc_var *samp, *v, *ocol0, *current, *combined, *raw, *prev, *next;
  struct rslc_func *sample_efb, *f;
  uint32_t in_block, e, z, then_s, else_s;

  copy_ps_block(sb, &blk);
  rslc_b_quals_init(&q);
  sb_sampler(sb, &q, 0);
  samp = rslc_b_declare(sb->b, &q, "samp0",
                        rslc_b_sampler_type(sb->b, RSLC_B_SAMPLER_2D_ARRAY, RSLC_BASE_FLOAT), 0);

  // A depth copy reads the first layer alone, with stereo's mono depth.
  in_types[0] = sb->f[3];
  in_types[1] = sb->f[1];
  sample_efb = rslc_b_func(sb->b, "SampleEFB", sb->u[4], 2, in_types, in_dirs, in_names);
  rslc_b_func_begin(sb->b, sample_efb);
  {
    struct rslc_var* uv = rslc_b_param(sample_efb, 0);
    struct rslc_var* y_offset = rslc_b_param(sample_efb, 1);
    struct rslc_var *tex_sample, *depth;
    z = sb_swz(sb, sb_ref(sb, uv), "z");
    if (g_ActiveConfig.bStereoEFBMonoDepth)
      z = rslc_b_select(sb->b, copy_flag(sb, &blk, 16u), sb_f(sb, 0.0), z);
    e = sb_call(sb, "clamp", 3,
                sb_bin(sb, RSLC_B_ADD, sb_swz(sb, sb_ref(sb, uv), "y"),
                       sb_bin(sb, RSLC_B_MUL, sb_ref(sb, y_offset), sb_ref(sb, blk.m[5]))),
                sb_swz(sb, sb_ref(sb, blk.m[4]), "x"), sb_swz(sb, sb_ref(sb, blk.m[4]), "y"));
    tex_sample = rslc_b_local(
        sb->b, "tex_sample", sb->f[4],
        sb_call(sb, "texture", 2, sb_ref(sb, samp),
                sb_con(sb, sb->f[3], 3, sb_swz(sb, sb_ref(sb, uv), "x"), e, z)),
        false);
    rslc_b_begin_block(sb->b);
    if (!g_backend_info.bSupportsReversedDepthRange)
      sb_set(sb, sb_swz(sb, sb_ref(sb, tex_sample), "x"),
             sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), sb_swz(sb, sb_ref(sb, tex_sample), "x")));
    depth = rslc_b_local(sb->b, "depth", sb->u[1],
                         sb_con(sb, sb->u[1], 1,
                                sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, tex_sample), "x"),
                                       sb_f(sb, 16777216.0))),
                         false);
    rslc_b_stmt(
        sb->b,
        rslc_b_return(
            sb->b,
            sb_con(sb, sb->u[4], 4,
                   sb_bin(sb, RSLC_B_BAND,
                          sb_bin(sb, RSLC_B_SHR, sb_ref(sb, depth), sb_i(sb, 16)), sb_u(sb, 255u)),
                   sb_bin(sb, RSLC_B_BAND,
                          sb_bin(sb, RSLC_B_SHR, sb_ref(sb, depth), sb_i(sb, 8)), sb_u(sb, 255u)),
                   sb_bin(sb, RSLC_B_BAND, sb_ref(sb, depth), sb_u(sb, 255u)), sb_u(sb, 255u))));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, copy_flag(sb, &blk, 16u), then_s, 0));
    rslc_b_stmt(sb->b,
                rslc_b_return(sb->b, sb_con(sb, sb->u[4], 1,
                                            sb_bin(sb, RSLC_B_MUL, sb_ref(sb, tex_sample),
                                                   sb_f(sb, 255.0)))));
  }
  rslc_b_func_end(sb->b);

  in_block = copy_v_tex0(sb, RSLC_B_ST_IN, &v);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.location = sb_output_location(sb, 0);
  ocol0 = rslc_b_declare(sb->b, &q, "ocol0", sb->f[4], 0);

  f = rslc_b_func(sb->b, "main", sb->v, 0, NULL, NULL, NULL);
  rslc_b_func_begin(sb->b, f);
  {
    uint32_t a[2];
    // The copy filter applies to both color and depth copies. This has been verified on
    // hardware. The filter is only applied to the RGB channels, the alpha channel is left intact.
    a[0] = copy_tex0(sb, v, in_block);
    a[1] = sb_f(sb, 0.0);
    current = rslc_b_local(sb->b, "current_row", sb->u[4], rslc_b_call(sb->b, sample_efb, a, 2),
                           false);
    combined = rslc_b_local(sb->b, "combined_rows", sb->u[3], 0, false);
    rslc_b_begin_block(sb->b);
    a[0] = copy_tex0(sb, v, in_block);
    a[1] = sb_un(sb, RSLC_B_NEG, sb_f(sb, 1.0));
    prev = rslc_b_local(sb->b, "prev_row", sb->u[4], rslc_b_call(sb->b, sample_efb, a, 2), false);
    a[0] = copy_tex0(sb, v, in_block);
    a[1] = sb_f(sb, 1.0);
    next = rslc_b_local(sb->b, "next_row", sb->u[4], rslc_b_call(sb->b, sample_efb, a, 2), false);
    e = sb_bin(sb, RSLC_B_ADD,
               sb_bin(sb, RSLC_B_ADD,
                      sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, prev), "rgb"),
                             sb_idx(sb, sb_ref(sb, blk.m[2]), sb_i(sb, 0))),
                      sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, current), "rgb"),
                             sb_idx(sb, sb_ref(sb, blk.m[2]), sb_i(sb, 1)))),
               sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, next), "rgb"),
                      sb_idx(sb, sb_ref(sb, blk.m[2]), sb_i(sb, 2))));
    sb_set(sb, sb_ref(sb, combined), e);
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, combined),
           sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, current), "rgb"),
                  sb_idx(sb, sb_ref(sb, blk.m[2]), sb_i(sb, 1))));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, copy_flag(sb, &blk, 1u), then_s, else_s));

    // Shift right by 6 to divide by 64, as filter coefficients
    // that sum to 64 result in no change in brightness
    raw = rslc_b_local(
        sb->b, "texcol_raw", sb->u[4],
        sb_con(sb, sb->u[4], 2,
               sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, combined), "rgb"), sb_i(sb, 6)),
               rslc_b_select(sb->b, copy_flag(sb, &blk, 2u), sb_swz(sb, sb_ref(sb, current), "a"),
                             sb_u(sb, 255u))),
        false);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, copy_flag(sb, &blk, 4u),
                          rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_BAND, sb_ref(sb, raw),
                                                           sb_u(sb, 0x1ffu))),
                          0));
    // Note that overflow occurs when the sum of values is >= 128, but this max situation can be
    // hit on >= 64, so we always include it.
    sb_set(sb, sb_ref(sb, raw),
           sb_call(sb, "min", 2, sb_ref(sb, raw),
                   sb_con(sb, sb->u[4], 4, sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255),
                          sb_i(sb, 255))));
    e = sb_call(sb, "pow", 2,
                sb_call(sb, "abs", 1,
                        sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, sb_ref(sb, raw)),
                               sb_f(sb, 255.0))),
                sb_con(sb, sb->f[4], 4, sb_ref(sb, blk.m[3]), sb_ref(sb, blk.m[3]),
                       sb_ref(sb, blk.m[3]), sb_f(sb, 1.0)));
    e = sb_con(sb, sb->u[4], 1,
               sb_call(sb, "round", 1, sb_bin(sb, RSLC_B_MUL, e, sb_f(sb, 255.0))));
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, copy_flag(sb, &blk, 8u),
                          rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, sb_ref(sb, raw), e)),
                          0));

    rslc_b_begin_block(sb->b);
    {
      // Intensity/YUV format conversion constants determined by hardware testing
      static const int32_t yuv[3][4] = {{66, 129, 25, 16}, {-38, -74, 112, 128},
                                        {112, -94, -18, 128}};
      static const char* const yuv_names[3] = {"y_const", "u_const", "v_const"};
      struct rslc_var* cst[3];
      uint32_t d[3];
      int k, j;
      for (k = 0; k < 3; k++)
      {
        uint32_t c[4];
        for (j = 0; j < 4; j++)
          c[j] = yuv[k][j] < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -yuv[k][j])) : sb_i(sb, yuv[k][j]);
        cst[k] = rslc_b_local(sb->b, yuv_names[k], sb->f[4],
                              rslc_b_construct(sb->b, sb->f[4], c, 4), true);
      }
      // Intensity/YUV format conversion
      for (k = 0; k < 3; k++)
        d[k] = sb_call(sb, "dot", 2, sb_ref(sb, cst[k]),
                       sb_con(sb, sb->f[4], 2, sb_swz(sb, sb_ref(sb, raw), "rgb"), sb_i(sb, 256)));
      sb_set(sb, sb_swz(sb, sb_ref(sb, raw), "rgb"), rslc_b_construct(sb->b, sb->u[3], d, 3));
      // Divide by 256 and round .5 and higher up
      sb_set(sb, sb_swz(sb, sb_ref(sb, raw), "rgb"),
             sb_bin(sb, RSLC_B_ADD,
                    sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, raw), "rgb"), sb_i(sb, 8)),
                    sb_bin(sb, RSLC_B_BAND,
                           sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, raw), "rgb"), sb_i(sb, 7)),
                           sb_u(sb, 1u))));
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, copy_flag(sb, &blk, 32u), then_s, 0));

    if (format)
    {
      copy_format_body(sb, raw, ocol0, *format);
    }
    else
    {
      // The format: each channel is a component of texcol_raw, masked to the bits the format
      // keeps and scaled by the mask, which is the largest value they hold; or 1.
      struct rslc_var* source =
          rslc_b_local(sb->b, "source", sb->u[4], copy_fields(sb, &blk, 8u, 10u, 12u, 14u, 3u),
                       false);
      struct rslc_var *picked, *mask, *one;
      picked = rslc_b_local(sb->b, "picked", sb->u[4],
                            sb_con(sb, sb->u[4], 4, copy_pick(sb, source, "x", raw),
                                   copy_pick(sb, source, "y", raw), copy_pick(sb, source, "z", raw),
                                   copy_pick(sb, source, "w", raw)),
                            false);
      mask = rslc_b_local(sb->b, "mask", sb->u[4], copy_fields(sb, &blk, 16u, 19u, 22u, 25u, 7u),
                          false);
      sb_set(sb, sb_ref(sb, mask),
             sb_con(sb, sb->u[4], 4, copy_mask_of(sb, mask, "x"), copy_mask_of(sb, mask, "y"),
                    copy_mask_of(sb, mask, "z"), copy_mask_of(sb, mask, "w")));
      one = rslc_b_local(sb->b, "one", sb->f[4],
                         sb_con(sb, sb->f[4], 1, copy_fields(sb, &blk, 28u, 29u, 30u, 31u, 1u)),
                         false);
      sb_set(sb, sb_ref(sb, ocol0),
             sb_call(sb, "mix", 3,
                     sb_bin(sb, RSLC_B_DIV,
                            sb_con(sb, sb->f[4], 1,
                                   sb_bin(sb, RSLC_B_BAND, sb_ref(sb, picked), sb_ref(sb, mask))),
                            sb_con(sb, sb->f[4], 1, sb_ref(sb, mask))),
                     sb_con(sb, sb->f[4], 4, sb_f(sb, 1.0), sb_f(sb, 1.0), sb_f(sb, 1.0),
                            sb_f(sb, 1.0)),
                     sb_ref(sb, one)));
    }
  }
  return rslc_b_func_end(sb->b);
}

}  // namespace TextureConversionShaderGen

