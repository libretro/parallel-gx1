// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureConversionShader.h"

#include <cstring>
#include <map>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/ShaderBuild.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace TextureConversionShaderTiled
{
u16 GetEncodedSampleCount(EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
    return 8;
  case EFBCopyFormat::RA4:
    return 4;
  case EFBCopyFormat::RA8:
    return 2;
  case EFBCopyFormat::RGB565:
    return 2;
  case EFBCopyFormat::RGB5A3:
    return 2;
  case EFBCopyFormat::RGBA8:
    return 1;
  case EFBCopyFormat::A8:
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
  case EFBCopyFormat::G8:
  case EFBCopyFormat::B8:
    return 4;
  case EFBCopyFormat::RG8:
  case EFBCopyFormat::GB8:
    return 2;
  case EFBCopyFormat::XFB:
    return 2;
  default:
    PanicAlert("Invalid EFB Copy Format %u! (GetEncodedSampleCount)",
               static_cast<unsigned>(format));
    return 1;
  }
}

/* The rest of the copy, packed into copy_flags: the EFB's format in bits 0-2, then depth, YUV,
 * the filter's three rows, its overflow and gamma, each a bit. */
u32 GetEncodingFlags(const EFBCopyParams& params)
{
  return (static_cast<u32>(params.efb_format) & 7u) | (params.depth ? 8u : 0u) |
         (params.yuv ? 16u : 0u) | (params.all_copy_filter_coefs_needed ? 32u : 0u) |
         (params.copy_filter_can_overflow ? 64u : 0u) | (params.apply_gamma ? 128u : 0u);
}

/* ---- the encoder, built by calls ---- */

namespace
{
/* PSBlock's members */
enum
{
  EC_POSITION,
  EC_Y_SCALE,
  EC_GAMMA_RCP,
  EC_CLAMP_TB,
  EC_FILTER_COEFFICIENTS,
  EC_COPY_FLAGS,
  EC_COUNT
};

struct ec_build
{
  const struct shader_build* sb;
  APIType api_type;
  struct rslc_var* ps[EC_COUNT];
  struct rslc_var* samp0;
  struct rslc_var* ocol0;
  struct rslc_var* uv0;
  struct rslc_var* pixel_size;
  struct rslc_func* to_rgb8;
  struct rslc_func* to_rgba6;
  struct rslc_func* to_rgb565;
  struct rslc_func* sample_efb0;
  struct rslc_func* sample_efb;
};

uint32_t ec_r(const struct shader_build* sb, const struct rslc_var* v)
{
  return sb_ref(sb, v);
}

uint32_t ec_swz(const struct shader_build* sb, const struct rslc_var* v, const char* c)
{
  return sb_swz(sb, sb_ref(sb, v), c);
}

/* a literal as the text writes it: negative ones as a negated literal */
uint32_t ec_int(const struct shader_build* sb, int v)
{
  return v < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -v)) : sb_i(sb, v);
}

uint32_t ec_float(const struct shader_build* sb, double v)
{
  return v < 0 ? sb_un(sb, RSLC_B_NEG, sb_f(sb, -v)) : sb_f(sb, v);
}

/* (copy_flags & bit) != 0u */
uint32_t ec_flag(const struct ec_build* e, u32 bit)
{
  const struct shader_build* sb = e->sb;
  return sb_bin(sb, RSLC_B_NE,
                sb_bin(sb, RSLC_B_BAND, ec_r(sb, e->ps[EC_COPY_FLAGS]), sb_u(sb, bit)),
                sb_u(sb, 0));
}

uint32_t ec_set_stmt(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  return rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r));
}

uint32_t ec_call1(const struct shader_build* sb, const struct rslc_func* f, uint32_t x)
{
  return rslc_b_call(sb->b, f, &x, 1);
}

uint32_t ec_call4(const struct shader_build* sb, const struct rslc_func* f, uint32_t a0,
                  uint32_t a1, uint32_t a2, uint32_t a3)
{
  uint32_t a[4];
  a[0] = a0;
  a[1] = a1;
  a[2] = a2;
  a[3] = a3;
  return rslc_b_call(sb->b, f, a, 4);
}

struct rslc_func* ec_func(const struct shader_build* sb, const char* name,
                          const struct rslc_type* ret, unsigned n,
                          const struct rslc_type* const* types, const char* const* names)
{
  static const unsigned dirs[4] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  struct rslc_func* f = rslc_b_func(sb->b, name, ret, n, types, dirs, names);
  if (f)
    rslc_b_func_begin(sb->b, f);
  return f;
}

/* float4(a, b, c, d) of int literals */
uint32_t ec_f4i(const struct shader_build* sb, int a, int b, int c, int d)
{
  return sb_con(sb, sb->f[4], 4, ec_int(sb, a), ec_int(sb, b), ec_int(sb, c), ec_int(sb, d));
}

/* Alpha channel in the copy is set to 1 the EFB format does not have an alpha channel. */
void ec_format_functions(struct ec_build* e)
{
  static const char* const names[1] = {"src"};
  const struct shader_build* sb = e->sb;
  const struct rslc_type* types[1];
  struct rslc_var *src, *val;
  int k;

  types[0] = sb->f[4];
  e->to_rgb8 = ec_func(sb, "RGBA8ToRGB8", sb->f[4], 1, types, names);
  src = rslc_b_param(e->to_rgb8, 0);
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, sb_con(sb, sb->f[4], 2, ec_swz(sb, src, "xyz"),
                                                   sb_f(sb, 1.0))));
  rslc_b_func_end(sb->b);

  for (k = 0; k < 2; k++)
  {
    struct rslc_func* f =
        ec_func(sb, k ? "RGBA8ToRGB565" : "RGBA8ToRGBA6", sb->f[4], 1, types, names);
    src = rslc_b_param(f, 0);
    val = rslc_b_local(
        sb->b, "val", sb->i[4],
        sb_con(sb, sb->i[4], 1,
               sb_call(sb, "roundEven", 1,
                       sb_bin(sb, RSLC_B_MUL, ec_r(sb, src), sb_f(sb, 255.0)))),
        false);
    if (!k)
    {
      sb_set(sb, ec_r(sb, val),
             sb_bin(sb, RSLC_B_BOR, sb_bin(sb, RSLC_B_BAND, ec_r(sb, val), sb_i(sb, 0xfc)),
                    sb_bin(sb, RSLC_B_SHR, ec_r(sb, val), sb_i(sb, 6))));
    }
    else
    {
      static const char* const comps[3] = {"r", "g", "b"};
      static const int masks[3] = {0xf8, 0xfc, 0xf8};
      static const int shifts[3] = {5, 6, 5};
      int c;
      for (c = 0; c < 3; c++)
        sb_set(sb, ec_swz(sb, val, comps[c]),
               sb_bin(sb, RSLC_B_BOR,
                      sb_bin(sb, RSLC_B_BAND, ec_swz(sb, val, comps[c]), sb_i(sb, masks[c])),
                      sb_bin(sb, RSLC_B_SHR, ec_swz(sb, val, comps[c]), sb_i(sb, shifts[c]))));
      sb_set(sb, ec_swz(sb, val, "a"), sb_i(sb, 255));
    }
    rslc_b_stmt(sb->b,
                rslc_b_return(sb->b, sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, ec_r(sb, val)),
                                            sb_f(sb, 255.0))));
    rslc_b_func_end(sb->b);
    if (k)
      e->to_rgb565 = f;
    else
      e->to_rgba6 = f;
  }
}

/* SampleEFB0 and SampleEFB: the rest of the copy, read from copy_flags (GetEncodingFlags): the
 * EFB's format in bits 0-2, then depth, YUV, the filter's three rows, its overflow and gamma,
 * each a bit. */
void ec_sample_functions(struct ec_build* e)
{
  static const char* const names0[4] = {"uv", "pixel_size", "x_offset", "y_offset"};
  static const char* const names[3] = {"uv", "pixel_size", "x_offset"};
  const struct shader_build* sb = e->sb;
  const struct rslc_type* types[4];
  struct rslc_func* f;
  struct rslc_var *uv, *ps, *xo, *yo, *tex_sample, *efb_format, *depth;
  uint32_t y, then_s, a[4];

  types[0] = types[1] = sb->f[2];
  types[2] = types[3] = sb->f[1];
  f = ec_func(sb, "SampleEFB0", sb->u[4], 4, types, names0);
  uv = rslc_b_param(f, 0);
  ps = rslc_b_param(f, 1);
  xo = rslc_b_param(f, 2);
  yo = rslc_b_param(f, 3);
  // Reverse the direction for OpenGL, since positive numbers are distance from the bottom row.
  y = sb_bin(sb, e->api_type == APIType::OpenGL ? RSLC_B_SUB : RSLC_B_ADD, ec_swz(sb, uv, "y"),
             sb_bin(sb, RSLC_B_MUL, ec_r(sb, yo), ec_swz(sb, ps, "y")));
  a[0] = ec_r(sb, e->samp0);
  a[1] = sb_con(sb, sb->f[3], 3,
                sb_bin(sb, RSLC_B_ADD, ec_swz(sb, uv, "x"),
                       sb_bin(sb, RSLC_B_MUL, ec_r(sb, xo), ec_swz(sb, ps, "x"))),
                sb_call(sb, "clamp", 3, y, ec_swz(sb, e->ps[EC_CLAMP_TB], "x"),
                        ec_swz(sb, e->ps[EC_CLAMP_TB], "y")),
                sb_f(sb, 0.0));
  tex_sample = rslc_b_local(sb->b, "tex_sample", sb->f[4],
                            rslc_b_builtin(sb->b, "texture", a, 2), false);
  efb_format = rslc_b_local(sb->b, "efb_format", sb->u[1],
                            sb_bin(sb, RSLC_B_BAND, ec_r(sb, e->ps[EC_COPY_FLAGS]), sb_u(sb, 7)),
                            false);
#define EC_FMT(fmt, fn)                                                                           \
  sb_bin(sb, RSLC_B_EQ, ec_r(sb, efb_format), sb_u(sb, static_cast<u32>(PixelFormat::fmt))),     \
      ec_set_stmt(sb, ec_r(sb, tex_sample), ec_call1(sb, e->fn, ec_r(sb, tex_sample)))
  {
    uint32_t c565 = rslc_b_if(sb->b, EC_FMT(RGB565_Z16, to_rgb565), 0);
    uint32_t c6 = rslc_b_if(sb->b, EC_FMT(RGBA6_Z24, to_rgba6), c565);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, EC_FMT(RGB8_Z24, to_rgb8), c6));
  }
#undef EC_FMT
  rslc_b_begin_block(sb->b);
  if (!g_backend_info.bSupportsReversedDepthRange)
    sb_set(sb, ec_swz(sb, tex_sample, "x"),
           sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), ec_swz(sb, tex_sample, "x")));
  depth = rslc_b_local(
      sb->b, "depth", sb->u[1],
      sb_con(sb, sb->u[1], 1,
             sb_bin(sb, RSLC_B_MUL, ec_swz(sb, tex_sample, "x"), sb_f(sb, 16777216.0))),
      false);
  rslc_b_stmt(
      sb->b,
      rslc_b_return(
          sb->b,
          sb_con(sb, sb->u[4], 4,
                 sb_bin(sb, RSLC_B_BAND, sb_bin(sb, RSLC_B_SHR, ec_r(sb, depth), sb_i(sb, 16)),
                        sb_u(sb, 255)),
                 sb_bin(sb, RSLC_B_BAND, sb_bin(sb, RSLC_B_SHR, ec_r(sb, depth), sb_i(sb, 8)),
                        sb_u(sb, 255)),
                 sb_bin(sb, RSLC_B_BAND, ec_r(sb, depth), sb_u(sb, 255)), sb_u(sb, 255))));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_if(sb->b, ec_flag(e, 8), then_s, 0));
  rslc_b_stmt(sb->b,
              rslc_b_return(sb->b,
                            sb_con(sb, sb->u[4], 1,
                                   sb_bin(sb, RSLC_B_MUL, ec_r(sb, tex_sample), sb_f(sb, 255.0)))));
  rslc_b_func_end(sb->b);
  e->sample_efb0 = f;

  // The copy filter applies to both color and depth copies. This has been verified on hardware.
  // The filter is only applied to the RGB channels, the alpha channel is left intact.
  types[2] = sb->i[1];
  f = ec_func(sb, "SampleEFB", sb->f[4], 3, types, names);
  {
    struct rslc_var *current_row, *combined, *prev_row, *next_row, *raw;
    uint32_t fc;
#define ROW(yoff)                                                                                 \
  ec_call4(sb, e->sample_efb0, ec_r(sb, rslc_b_param(f, 0)), ec_r(sb, rslc_b_param(f, 1)),     \
             sb_con(sb, sb->f[1], 1, ec_r(sb, rslc_b_param(f, 2))), ec_float(sb, yoff))
#define FC(i) sb_idx(sb, ec_r(sb, e->ps[EC_FILTER_COEFFICIENTS]), sb_i(sb, i))
    current_row = rslc_b_local(sb->b, "current_row", sb->u[4], ROW(0.0), false);
    combined = rslc_b_local(sb->b, "combined_rows", sb->u[3],
                            sb_bin(sb, RSLC_B_MUL, ec_swz(sb, current_row, "rgb"), FC(1)), false);
    rslc_b_begin_block(sb->b);
    prev_row = rslc_b_local(sb->b, "prev_row", sb->u[4], ROW(-1.0), false);
    next_row = rslc_b_local(sb->b, "next_row", sb->u[4], ROW(1.0), false);
    sb_op_set(sb, RSLC_B_ADD, ec_r(sb, combined),
              sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_MUL, ec_swz(sb, prev_row, "rgb"), FC(0)),
                     sb_bin(sb, RSLC_B_MUL, ec_swz(sb, next_row, "rgb"), FC(2))));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, ec_flag(e, 32), then_s, 0));
#undef ROW
#undef FC
    // Shift right by 6 to divide by 64, as filter coefficients
    // that sum to 64 result in no change in brightness
    raw = rslc_b_local(sb->b, "texcol_raw", sb->u[4],
                       sb_con(sb, sb->u[4], 2,
                              sb_bin(sb, RSLC_B_SHR, ec_swz(sb, combined, "rgb"), sb_i(sb, 6)),
                              ec_swz(sb, current_row, "a")),
                       false);
    rslc_b_stmt(sb->b,
                rslc_b_if(sb->b, ec_flag(e, 64),
                          rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_BAND, ec_r(sb, raw),
                                                           sb_u(sb, 0x1ff))),
                          0));
    // Note that overflow occurs when the sum of values is >= 128, but this max situation can be
    // hit on >= 64, so we always include it.
    sb_set(sb, ec_r(sb, raw),
           sb_call(sb, "min", 2, ec_r(sb, raw),
                   sb_con(sb, sb->u[4], 4, sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255),
                          sb_i(sb, 255))));
    rslc_b_begin_block(sb->b);
    fc = sb_con(sb, sb->f[4], 4, ec_r(sb, e->ps[EC_GAMMA_RCP]), ec_r(sb, e->ps[EC_GAMMA_RCP]),
                ec_r(sb, e->ps[EC_GAMMA_RCP]), sb_f(sb, 1.0));
    sb_set(sb, ec_r(sb, raw),
           sb_con(sb, sb->u[4], 1,
                  sb_call(sb, "round", 1,
                          sb_bin(sb, RSLC_B_MUL,
                                 sb_call(sb, "pow", 2,
                                         sb_bin(sb, RSLC_B_DIV,
                                                sb_con(sb, sb->f[4], 1, ec_r(sb, raw)),
                                                sb_f(sb, 255.0)),
                                         fc),
                                 sb_f(sb, 255.0)))));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, ec_flag(e, 128), then_s, 0));
    rslc_b_begin_block(sb->b);
    {
      struct rslc_var* k[3];
      uint32_t d[3];
      int c;
      // Intensity/YUV format conversion constants determined by hardware testing
      k[0] = rslc_b_local(sb->b, "y_const", sb->f[4], ec_f4i(sb, 66, 129, 25, 16), true);
      k[1] = rslc_b_local(sb->b, "u_const", sb->f[4], ec_f4i(sb, -38, -74, 112, 128), true);
      k[2] = rslc_b_local(sb->b, "v_const", sb->f[4], ec_f4i(sb, 112, -94, -18, 128), true);
      // Intensity/YUV format conversion
      for (c = 0; c < 3; c++)
        d[c] = sb_call(sb, "dot", 2, ec_r(sb, k[c]),
                       sb_con(sb, sb->f[4], 2, ec_swz(sb, raw, "rgb"), sb_i(sb, 256)));
      sb_set(sb, ec_swz(sb, raw, "rgb"), sb_con(sb, sb->u[3], 3, d[0], d[1], d[2]));
      // Divide by 256 and round .5 and higher up
      sb_set(sb, ec_swz(sb, raw, "rgb"),
             sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_SHR, ec_swz(sb, raw, "rgb"), sb_i(sb, 8)),
                    sb_bin(sb, RSLC_B_BAND,
                           sb_bin(sb, RSLC_B_SHR, ec_swz(sb, raw, "rgb"), sb_i(sb, 7)),
                           sb_u(sb, 1))));
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, ec_flag(e, 16), then_s, 0));
    rslc_b_stmt(sb->b,
                rslc_b_return(sb->b, sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[4], 1, ec_r(sb, raw)),
                                            sb_f(sb, 255.0))));
  }
  rslc_b_func_end(sb->b);
  e->sample_efb = f;
}

/* SampleEFB(uv0, pixel_size, x_offset).comp */
uint32_t ec_sample(const struct ec_build* e, int x_offset, const char* comp)
{
  const struct shader_build* sb = e->sb;
  uint32_t a[3];
  a[0] = ec_r(sb, e->uv0);
  a[1] = ec_r(sb, e->pixel_size);
  a[2] = sb_i(sb, x_offset);
  return sb_swz(sb, rslc_b_call(sb->b, e->sample_efb, a, 3), comp);
}

/* floor(src * 255.0 / exp2(8.0 - depth.0)) */
uint32_t ec_bit_depth(const struct shader_build* sb, int depth, uint32_t src)
{
  return sb_call(sb, "floor", 1,
                 sb_bin(sb, RSLC_B_DIV, sb_bin(sb, RSLC_B_MUL, src, sb_f(sb, 255.0)),
                        sb_call(sb, "exp2", 1,
                                sb_bin(sb, RSLC_B_SUB, sb_f(sb, 8.0), sb_f(sb, depth)))));
}

uint32_t ec_o(const struct ec_build* e, const char* c)
{
  return ec_swz(e->sb, e->ocol0, c);
}

/* ocol0 = ocol0 / 255.0; */
void ec_normalize(const struct ec_build* e)
{
  const struct shader_build* sb = e->sb;
  sb_set(sb, ec_r(sb, e->ocol0), sb_bin(sb, RSLC_B_DIV, ec_r(sb, e->ocol0), sb_f(sb, 255.0)));
}

void ec_rgb565(const struct ec_build* e)
{
  const struct shader_build* sb = e->sb;
  struct rslc_var *s0, *s1, *rs, *gs, *bs, *g_int, *g_upper, *g_lower;
  s0 = rslc_b_local(sb->b, "texSample0", sb->f[3], 0, false);
  s1 = rslc_b_local(sb->b, "texSample1", sb->f[3], 0, false);
  sb_set(sb, ec_r(sb, s0), ec_sample(e, 0, "rgb"));
  sb_set(sb, ec_r(sb, s1), ec_sample(e, 1, "rgb"));
  rs = rslc_b_local(sb->b, "texRs", sb->f[2],
                    sb_con(sb, sb->f[2], 2, ec_swz(sb, s0, "r"), ec_swz(sb, s1, "r")), false);
  gs = rslc_b_local(sb->b, "texGs", sb->f[2],
                    sb_con(sb, sb->f[2], 2, ec_swz(sb, s0, "g"), ec_swz(sb, s1, "g")), false);
  bs = rslc_b_local(sb->b, "texBs", sb->f[2],
                    sb_con(sb, sb->f[2], 2, ec_swz(sb, s0, "b"), ec_swz(sb, s1, "b")), false);
  g_int = rslc_b_local(sb->b, "gInt", sb->f[2], ec_bit_depth(sb, 6, ec_r(sb, gs)), false);
  g_upper = rslc_b_local(sb->b, "gUpper", sb->f[2],
                         sb_call(sb, "floor", 1,
                                 sb_bin(sb, RSLC_B_DIV, ec_r(sb, g_int), sb_f(sb, 8.0))),
                         false);
  g_lower = rslc_b_local(
      sb->b, "gLower", sb->f[2],
      sb_bin(sb, RSLC_B_SUB, ec_r(sb, g_int),
             sb_bin(sb, RSLC_B_MUL, ec_r(sb, g_upper), sb_f(sb, 8.0))),
      false);
  sb_set(sb, ec_o(e, "br"), ec_bit_depth(sb, 5, ec_r(sb, rs)));
  sb_set(sb, ec_o(e, "br"),
         sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_MUL, ec_o(e, "br"), sb_f(sb, 8.0)),
                ec_r(sb, g_upper)));
  sb_set(sb, ec_o(e, "ga"), ec_bit_depth(sb, 5, ec_r(sb, bs)));
  sb_set(sb, ec_o(e, "ga"),
         sb_bin(sb, RSLC_B_ADD, ec_o(e, "ga"),
                sb_bin(sb, RSLC_B_MUL, ec_r(sb, g_lower), sb_f(sb, 32.0))));
  ec_normalize(e);
}

/* one RGB5A3 texel into ocol0's c_hi and c_lo */
void ec_rgb5a3_texel(const struct ec_build* e, struct rslc_var* tex, struct rslc_var* color0,
                     struct rslc_var* g_upper, struct rslc_var* g_lower, const char* c_hi,
                     const char* c_lo)
{
  const struct shader_build* sb = e->sb;
  uint32_t then_s, else_s;
  // 0.8784 = 224 / 255 which is the maximum alpha value that can be represented in 3 bits
  rslc_b_begin_block(sb->b);
  sb_set(sb, ec_r(sb, color0), ec_bit_depth(sb, 5, ec_swz(sb, tex, "g")));
  sb_set(sb, ec_r(sb, g_upper),
         sb_call(sb, "floor", 1, sb_bin(sb, RSLC_B_DIV, ec_r(sb, color0), sb_f(sb, 8.0))));
  sb_set(sb, ec_r(sb, g_lower),
         sb_bin(sb, RSLC_B_SUB, ec_r(sb, color0),
                sb_bin(sb, RSLC_B_MUL, ec_r(sb, g_upper), sb_f(sb, 8.0))));
  sb_set(sb, ec_o(e, c_hi), ec_bit_depth(sb, 5, ec_swz(sb, tex, "r")));
  sb_set(sb, ec_o(e, c_hi),
         sb_bin(sb, RSLC_B_ADD,
                sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_MUL, ec_o(e, c_hi), sb_f(sb, 4.0)),
                       ec_r(sb, g_upper)),
                sb_f(sb, 128.0)));
  sb_set(sb, ec_o(e, c_lo), ec_bit_depth(sb, 5, ec_swz(sb, tex, "b")));
  sb_set(sb, ec_o(e, c_lo),
         sb_bin(sb, RSLC_B_ADD, ec_o(e, c_lo),
                sb_bin(sb, RSLC_B_MUL, ec_r(sb, g_lower), sb_f(sb, 32.0))));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_begin_block(sb->b);
  sb_set(sb, ec_o(e, c_hi), ec_bit_depth(sb, 4, ec_swz(sb, tex, "r")));
  sb_set(sb, ec_o(e, c_lo), ec_bit_depth(sb, 4, ec_swz(sb, tex, "b")));
  sb_set(sb, ec_r(sb, color0), ec_bit_depth(sb, 3, ec_swz(sb, tex, "a")));
  sb_set(sb, ec_o(e, c_hi),
         sb_bin(sb, RSLC_B_ADD, ec_o(e, c_hi),
                sb_bin(sb, RSLC_B_MUL, ec_r(sb, color0), sb_f(sb, 16.0))));
  sb_set(sb, ec_r(sb, color0), ec_bit_depth(sb, 4, ec_swz(sb, tex, "g")));
  sb_set(sb, ec_o(e, c_lo),
         sb_bin(sb, RSLC_B_ADD, ec_o(e, c_lo),
                sb_bin(sb, RSLC_B_MUL, ec_r(sb, color0), sb_f(sb, 16.0))));
  else_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_GT, ec_swz(sb, tex, "a"), sb_f(sb, 0.878)),
                        then_s, else_s));
}

void ec_rgb5a3(const struct ec_build* e)
{
  const struct shader_build* sb = e->sb;
  struct rslc_var *tex, *color0, *g_upper, *g_lower;
  tex = rslc_b_local(sb->b, "texSample", sb->f[4], 0, false);
  color0 = rslc_b_local(sb->b, "color0", sb->f[1], 0, false);
  g_upper = rslc_b_local(sb->b, "gUpper", sb->f[1], 0, false);
  g_lower = rslc_b_local(sb->b, "gLower", sb->f[1], 0, false);
  sb_set(sb, ec_r(sb, tex), ec_sample(e, 0, "rgba"));
  ec_rgb5a3_texel(e, tex, color0, g_upper, g_lower, "b", "g");
  sb_set(sb, ec_r(sb, tex), ec_sample(e, 1, "rgba"));
  ec_rgb5a3_texel(e, tex, color0, g_upper, g_lower, "r", "a");
  ec_normalize(e);
}

void ec_rgba8(const struct ec_build* e, struct rslc_var* first)
{
  const struct shader_build* sb = e->sb;
  struct rslc_var *tex, *color0, *color1;
  tex = rslc_b_local(sb->b, "texSample", sb->f[4], 0, false);
  color0 = rslc_b_local(sb->b, "color0", sb->f[4], 0, false);
  color1 = rslc_b_local(sb->b, "color1", sb->f[4], 0, false);
  sb_set(sb, ec_r(sb, tex), ec_sample(e, 0, "rgba"));
  sb_set(sb, ec_swz(sb, color0, "b"), ec_swz(sb, tex, "a"));
  sb_set(sb, ec_swz(sb, color0, "g"), ec_swz(sb, tex, "r"));
  sb_set(sb, ec_swz(sb, color1, "b"), ec_swz(sb, tex, "g"));
  sb_set(sb, ec_swz(sb, color1, "g"), ec_swz(sb, tex, "b"));
  sb_set(sb, ec_r(sb, tex), ec_sample(e, 1, "rgba"));
  sb_set(sb, ec_swz(sb, color0, "r"), ec_swz(sb, tex, "a"));
  sb_set(sb, ec_swz(sb, color0, "a"), ec_swz(sb, tex, "r"));
  sb_set(sb, ec_swz(sb, color1, "r"), ec_swz(sb, tex, "g"));
  sb_set(sb, ec_swz(sb, color1, "a"), ec_swz(sb, tex, "b"));
  sb_set(sb, ec_r(sb, e->ocol0),
         rslc_b_select(sb->b, ec_r(sb, first), ec_r(sb, color0), ec_r(sb, color1)));
}

/* color0, color1 to 4 bits; ocol0 = (color0 * 16.0 + color1) / 255.0; */
void ec_pack4(const struct ec_build* e, struct rslc_var* color0, struct rslc_var* color1)
{
  const struct shader_build* sb = e->sb;
  sb_set(sb, ec_r(sb, color0), ec_bit_depth(sb, 4, ec_r(sb, color0)));
  sb_set(sb, ec_r(sb, color1), ec_bit_depth(sb, 4, ec_r(sb, color1)));
  sb_set(sb, ec_r(sb, e->ocol0),
         sb_bin(sb, RSLC_B_DIV,
                sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_MUL, ec_r(sb, color0), sb_f(sb, 16.0)),
                       ec_r(sb, color1)),
                sb_f(sb, 255.0)));
}

void ec_c4(const struct ec_build* e, const char* comp)
{
  static const char* const dst[4] = {"b", "g", "r", "a"};
  const struct shader_build* sb = e->sb;
  struct rslc_var *color0, *color1;
  int k;
  color0 = rslc_b_local(sb->b, "color0", sb->f[4], 0, false);
  color1 = rslc_b_local(sb->b, "color1", sb->f[4], 0, false);
  for (k = 0; k < 8; k++)
    sb_set(sb, ec_swz(sb, (k & 1) ? color1 : color0, dst[k >> 1]), ec_sample(e, k, comp));
  ec_pack4(e, color0, color1);
}

void ec_c8(const struct ec_build* e, const char* comp)
{
  static const char* const dst[4] = {"b", "g", "r", "a"};
  int k;
  for (k = 0; k < 4; k++)
    sb_set(e->sb, ec_o(e, dst[k]), ec_sample(e, k, comp));
}

void ec_cc4(const struct ec_build* e, const char* comp)
{
  static const char* const dst[4] = {"b", "g", "r", "a"};
  const struct shader_build* sb = e->sb;
  struct rslc_var *tex, *color0, *color1;
  int k;
  tex = rslc_b_local(sb->b, "texSample", sb->f[2], 0, false);
  color0 = rslc_b_local(sb->b, "color0", sb->f[4], 0, false);
  color1 = rslc_b_local(sb->b, "color1", sb->f[4], 0, false);
  for (k = 0; k < 4; k++)
  {
    sb_set(sb, ec_r(sb, tex), ec_sample(e, k, comp));
    sb_set(sb, ec_swz(sb, color0, dst[k]), ec_swz(sb, tex, "x"));
    sb_set(sb, ec_swz(sb, color1, dst[k]), ec_swz(sb, tex, "y"));
  }
  ec_pack4(e, color0, color1);
}

void ec_cc8(const struct ec_build* e, const char* comp)
{
  sb_set(e->sb, ec_o(e, "bg"), ec_sample(e, 0, comp));
  sb_set(e->sb, ec_o(e, "ra"), ec_sample(e, 1, comp));
}

void ec_xfb(const struct ec_build* e)
{
  static const char* const dst[4] = {"b", "g", "r", "a"};
  const struct shader_build* sb = e->sb;
  struct rslc_var *color0, *color1, *k[3], *average;
  int c;
  color0 = rslc_b_local(sb->b, "color0", sb->f[4], ec_f4i(sb, 0, 0, 0, 1), false);
  color1 = rslc_b_local(sb->b, "color1", sb->f[4], ec_f4i(sb, 0, 0, 0, 1), false);
  sb_set(sb, ec_swz(sb, color0, "rgb"), ec_sample(e, 0, "rgb"));
  sb_set(sb, ec_swz(sb, color1, "rgb"), ec_sample(e, 1, "rgb"));
  // Convert to YUV. Intensity/YUV format conversion constants determined by hardware testing
  k[0] = rslc_b_local(sb->b, "y_const", sb->f[4], ec_f4i(sb, 66, 129, 25, 16), true);
  k[1] = rslc_b_local(sb->b, "u_const", sb->f[4], ec_f4i(sb, -38, -74, 112, 128), true);
  k[2] = rslc_b_local(sb->b, "v_const", sb->f[4], ec_f4i(sb, 112, -94, -18, 128), true);
  average = rslc_b_local(
      sb->b, "average", sb->f[4],
      sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_ADD, ec_r(sb, color0), ec_r(sb, color1)),
             sb_f(sb, 0.5)),
      false);
  // TODO: check rounding
  for (c = 0; c < 4; c++)
  {
    /* b: color0 y, g: average u, r: color1 y, a: average v */
    struct rslc_var* v = c == 0 ? color0 : c == 2 ? color1 : average;
    struct rslc_var* kc = c == 1 ? k[1] : c == 3 ? k[2] : k[0];
    sb_set(sb, ec_o(e, dst[c]),
           sb_bin(sb, RSLC_B_DIV,
                  sb_call(sb, "round", 1, sb_call(sb, "dot", 2, ec_r(sb, v), ec_r(sb, kc))),
                  sb_f(sb, 256.0)));
  }
}
}  // namespace

bool BuildEncodingShader(struct shader_build* sb, const void* arg)
{
  static const char* const names[EC_COUNT] = {"position",           "y_scale",   "gamma_rcp",
                                              "clamp_tb",           "filter_coefficients",
                                              "copy_flags"};
  const EncodingShaderBuildArgs* a = static_cast<const EncodingShaderBuildArgs*>(arg);
  const EFBCopyFormat format = a->format;
  const struct rslc_type* types[EC_COUNT];
  struct ec_build eb;
  struct ec_build* e = &eb;
  struct rslc_b_quals q;
  struct rslc_var *sample_uv, *uv1, *x_block, *y_block, *first = nullptr, *offset_in_block;
  struct rslc_var *y_off, *x_off;
  struct rslc_func* fmain;
  const int blkW = TexDecoder_GetEFBCopyBlockWidthInTexels(format);
  const int blkH = TexDecoder_GetEFBCopyBlockHeightInTexels(format);
  int samples = GetEncodedSampleCount(format);

  memset(e, 0, sizeof(*e));
  e->sb = sb;
  e->api_type = a->api_type;

  // left, top, of source rectangle within source texture
  // width of the destination rectangle, scale_factor (1 or 2)
  types[EC_POSITION] = sb->i[4];
  types[EC_Y_SCALE] = types[EC_GAMMA_RCP] = sb->f[1];
  types[EC_CLAMP_TB] = sb->f[2];
  types[EC_FILTER_COEFFICIENTS] = sb->u[3];
  types[EC_COPY_FLAGS] = sb->u[1];
  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 1);
  rslc_b_block_q(sb->b, &q, "PSBlock", nullptr, types, names, EC_COUNT, e->ps);
  {
    static const char* const vnames[1] = {"v_tex0"};
    const struct rslc_type* vtypes[1];
    vtypes[0] = sb->f[3];
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.location = sb_varying_location(sb, 0);
    if (g_backend_info.bSupportsGeometryShaders)
      rslc_b_io_block(sb->b, &q, "VertexData", nullptr, vtypes, vnames, nullptr, nullptr, 1, -1);
    else
      rslc_b_declare(sb->b, &q, "v_tex0", sb->f[3], 0);
  }
  rslc_b_quals_init(&q);
  sb_sampler(sb, &q, 0);
  e->samp0 = rslc_b_declare(
      sb->b, &q, "samp0",
      rslc_b_sampler_type(sb->b, RSLC_B_SAMPLER_2D_ARRAY, RSLC_BASE_FLOAT), 0);
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.location = sb_output_location(sb, 0);
  e->ocol0 = rslc_b_declare(sb->b, &q, "ocol0", sb->f[4], 0);

  ec_format_functions(e);
  ec_sample_functions(e);

  // Block dimensions   : widthStride, heightStride
  // Texture dimensions : width, height, x offset, y offset
  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);
  sample_uv = rslc_b_local(sb->b, "sampleUv", sb->i[2], 0, false);
  uv1 = rslc_b_local(sb->b, "uv1", sb->i[2],
                     sb_con(sb, sb->i[2], 1, sb_swz(sb, sb_bi(sb, "gl_FragCoord"), "xy")), false);
  x_block = rslc_b_local(
      sb->b, "x_block_position", sb->i[1],
      sb_bin(sb, RSLC_B_SHL,
             sb_bin(sb, RSLC_B_SHR, ec_swz(sb, uv1, "x"),
                    sb_i(sb, MathUtil::IntLog2(blkH * blkW / samples))),
             sb_i(sb, MathUtil::IntLog2(blkW))),
      false);
  y_block = rslc_b_local(sb->b, "y_block_position", sb->i[1],
                         sb_bin(sb, RSLC_B_SHL, ec_swz(sb, uv1, "y"),
                                sb_i(sb, MathUtil::IntLog2(blkH))),
                         false);
  if (samples == 1)
  {
    // With samples == 1, we write out pairs of blocks; one A8R8, one G8B8.
    first = rslc_b_local(
        sb->b, "first", sb->bl,
        sb_bin(sb, RSLC_B_EQ,
               sb_bin(sb, RSLC_B_BAND, ec_swz(sb, uv1, "x"), sb_i(sb, blkH * blkW / 2)),
               sb_i(sb, 0)),
        false);
    samples = 2;
  }
  offset_in_block = rslc_b_local(
      sb->b, "offset_in_block", sb->i[1],
      sb_bin(sb, RSLC_B_BAND, ec_swz(sb, uv1, "x"), sb_i(sb, (blkH * blkW / samples) - 1)), false);
  y_off = rslc_b_local(sb->b, "y_offset_in_block", sb->i[1],
                       sb_bin(sb, RSLC_B_SHR, ec_r(sb, offset_in_block),
                              sb_i(sb, MathUtil::IntLog2(blkW / samples))),
                       false);
  x_off = rslc_b_local(
      sb->b, "x_offset_in_block", sb->i[1],
      sb_bin(sb, RSLC_B_SHL,
             sb_bin(sb, RSLC_B_BAND, ec_r(sb, offset_in_block), sb_i(sb, (blkW / samples) - 1)),
             sb_i(sb, MathUtil::IntLog2(samples))),
      false);
  sb_set(sb, ec_swz(sb, sample_uv, "x"),
         sb_bin(sb, RSLC_B_ADD, ec_r(sb, x_block), ec_r(sb, x_off)));
  sb_set(sb, ec_swz(sb, sample_uv, "y"),
         sb_bin(sb, RSLC_B_ADD, ec_r(sb, y_block), ec_r(sb, y_off)));

  // sampleUv is the sample position in (int)gx_coords
  e->uv0 = rslc_b_local(sb->b, "uv0", sb->f[2], sb_con(sb, sb->f[2], 1, ec_r(sb, sample_uv)),
                        false);
  // Move to center of pixel
  sb_op_set(sb, RSLC_B_ADD, ec_r(sb, e->uv0),
            sb_con(sb, sb->f[2], 2, sb_f(sb, 0.5), sb_f(sb, 0.5)));
  // Scale by two if needed (also move to pixel borders so that linear filtering will average
  // adjacent pixel)
  sb_op_set(sb, RSLC_B_MUL, ec_r(sb, e->uv0),
            sb_con(sb, sb->f[1], 1, ec_swz(sb, e->ps[EC_POSITION], "w")));
  // Move to copied rect
  sb_op_set(sb, RSLC_B_ADD, ec_r(sb, e->uv0),
            sb_con(sb, sb->f[2], 1, ec_swz(sb, e->ps[EC_POSITION], "xy")));
  // Normalize to [0:1]
  sb_op_set(sb, RSLC_B_DIV, ec_r(sb, e->uv0),
            sb_con(sb, sb->f[2], 2, sb_i(sb, EFB_WIDTH), sb_i(sb, EFB_HEIGHT)));
  // Apply the y scaling
  sb_op_set(sb, RSLC_B_DIV, ec_r(sb, e->uv0),
            sb_con(sb, sb->f[2], 2, sb_i(sb, 1), ec_r(sb, e->ps[EC_Y_SCALE])));
  // OGL has to flip up and down
  if (a->api_type == APIType::OpenGL)
    sb_set(sb, ec_swz(sb, e->uv0, "y"),
           sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), ec_swz(sb, e->uv0, "y")));
  e->pixel_size = rslc_b_local(
      sb->b, "pixel_size", sb->f[2],
      sb_bin(sb, RSLC_B_DIV,
             sb_con(sb, sb->f[2], 2, ec_swz(sb, e->ps[EC_POSITION], "w"),
                    ec_swz(sb, e->ps[EC_POSITION], "w")),
             sb_con(sb, sb->f[2], 2, sb_i(sb, EFB_WIDTH), sb_i(sb, EFB_HEIGHT))),
      false);

  switch (format)
  {
  case EFBCopyFormat::R4:
    ec_c4(e, "r");
    break;
  case EFBCopyFormat::RA4:
    ec_cc4(e, "ar");
    break;
  case EFBCopyFormat::RA8:
    ec_cc8(e, "ar");
    break;
  case EFBCopyFormat::RGB565:
    ec_rgb565(e);
    break;
  case EFBCopyFormat::RGB5A3:
    ec_rgb5a3(e);
    break;
  case EFBCopyFormat::RGBA8:
    ec_rgba8(e, first);
    break;
  case EFBCopyFormat::A8:
    ec_c8(e, "a");
    break;
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
    ec_c8(e, "r");
    break;
  case EFBCopyFormat::G8:
    ec_c8(e, "g");
    break;
  case EFBCopyFormat::B8:
    ec_c8(e, "b");
    break;
  case EFBCopyFormat::RG8:
    ec_cc8(e, "gr");
    break;
  case EFBCopyFormat::GB8:
    ec_cc8(e, "bg");
    break;
  case EFBCopyFormat::XFB:
    ec_xfb(e);
    break;
  default:
    PanicAlert("Invalid EFB Copy Format %u! (BuildEncodingShader)", static_cast<unsigned>(format));
    break;
  }
  return rslc_b_func_end(sb->b);
}

// NOTE: In these uniforms, a row refers to a row of blocks, not texels.
static const char decoding_shader_header[] = R"(
#if defined(PALETTE_FORMAT_IA8) || defined(PALETTE_FORMAT_RGB565) || defined(PALETTE_FORMAT_RGB5A3)
#define HAS_PALETTE 1
#endif

UBO_BINDING(std140, 1) uniform UBO {
  uint2 u_dst_size;
  uint2 u_src_size;
  uint u_src_offset;
  uint u_src_row_stride;
  uint u_palette_offset;
};

#if defined(API_METAL)

// The buffers are read a word at a time: bytes and halves are picked out
// of it, the first in the low bits.
#if defined(TEXEL_BUFFER_FORMAT_R8)
  SSBO_BINDING(0) readonly buffer Input { uint s_input_buffer[]; };
  #define FETCH(offset) ((s_input_buffer[uint(offset) >> 2] >> ((uint(offset) & 3u) << 3)) & 0xFFu)
#elif defined(TEXEL_BUFFER_FORMAT_R16)
  SSBO_BINDING(0) readonly buffer Input { uint s_input_buffer[]; };
  #define FETCH(offset) ((s_input_buffer[uint(offset) >> 1] >> ((uint(offset) & 1u) << 4)) & 0xFFFFu)
#elif defined(TEXEL_BUFFER_FORMAT_RGBA8)
  SSBO_BINDING(0) readonly buffer Input { uint s_input_buffer[]; };
  #define FETCH(offset) ((uvec4(s_input_buffer[uint(offset)]) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu)
#elif defined(TEXEL_BUFFER_FORMAT_R32G32)
  SSBO_BINDING(0) readonly buffer Input { uvec2 s_input_buffer[]; };
  #define FETCH(offset) s_input_buffer[offset]
#else
  #error No texel buffer?
#endif

#ifdef HAS_PALETTE
  SSBO_BINDING(1) readonly buffer Palette { uint s_palette_buffer[]; };
  #define FETCH_PALETTE(offset) ((s_palette_buffer[uint(offset) >> 1] >> ((uint(offset) & 1u) << 4)) & 0xFFFFu)
#endif

#else

TEXEL_BUFFER_BINDING(0) uniform usamplerBuffer s_input_buffer;

#if defined(TEXEL_BUFFER_FORMAT_R8) || defined(TEXEL_BUFFER_FORMAT_R16)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset)).r
#elif defined(TEXEL_BUFFER_FORMAT_RGBA8)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset))
#elif defined(TEXEL_BUFFER_FORMAT_R32G32)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset)).rg
#else
  #error No texel buffer?
#endif

#ifdef HAS_PALETTE
  TEXEL_BUFFER_BINDING(1) uniform usamplerBuffer s_palette_buffer;
  #define FETCH_PALETTE(offset) texelFetch(s_palette_buffer, int((offset) + u_palette_offset)).r
#endif

#endif // defined(API_METAL)
IMAGE_BINDING(rgba8, 0) uniform writeonly image2DArray output_image;

#define GROUP_MEMORY_BARRIER_WITH_SYNC memoryBarrierShared(); barrier();
#define GROUP_SHARED shared

#define DEFINE_MAIN(lx, ly) \
  layout(local_size_x = lx, local_size_y = ly) in; \
  void main()

uint Swap16(uint v)
{
  // Convert BE to LE.
  return ((v >> 8) | (v << 8)) & 0xFFFFu;
}

uint Convert3To8(uint v)
{
  // Swizzle bits: 00000123 -> 12312312
  return (v << 5) | (v << 2) | (v >> 1);
}
uint Convert4To8(uint v)
{
  // Swizzle bits: 00001234 -> 12341234
  return (v << 4) | v;
}
uint Convert5To8(uint v)
{
  // Swizzle bits: 00012345 -> 12345123
  return (v << 3) | (v >> 2);
}
uint Convert6To8(uint v)
{
  // Swizzle bits: 00123456 -> 12345612
  return (v << 2) | (v >> 4);
}

uint GetTiledTexelOffset(uint2 block_size, uint2 coords)
{
  uint2 block = coords / block_size;
  uint2 offset = coords % block_size;
  uint buffer_pos = 0u;
  buffer_pos += block.y * u_src_row_stride;
  buffer_pos += block.x * (block_size.x * block_size.y);
  buffer_pos += offset.y * block_size.x;
  buffer_pos += offset.x;
  return buffer_pos;
}

#if defined(HAS_PALETTE)
uint4 GetPaletteColor(uint index)
{
  // Fetch and swap BE to LE.
  uint val = Swap16(FETCH_PALETTE(index));

  uint4 color;
#if defined(PALETTE_FORMAT_IA8)
  uint a = bitfieldExtract(val, 8, 8);
  uint i = bitfieldExtract(val, 0, 8);
  color = uint4(i, i, i, a);
#elif defined(PALETTE_FORMAT_RGB565)
  color.x = Convert5To8(bitfieldExtract(val, 11, 5));
  color.y = Convert6To8(bitfieldExtract(val, 5, 6));
  color.z = Convert5To8(bitfieldExtract(val, 0, 5));
  color.a = 255u;

#elif defined(PALETTE_FORMAT_RGB5A3)
  if ((val & 0x8000u) != 0u)
  {
    color.x = Convert5To8(bitfieldExtract(val, 10, 5));
    color.y = Convert5To8(bitfieldExtract(val, 5, 5));
    color.z = Convert5To8(bitfieldExtract(val, 0, 5));
    color.a = 255u;
  }
  else
  {
    color.a = Convert3To8(bitfieldExtract(val, 12, 3));
    color.r = Convert4To8(bitfieldExtract(val, 8, 4));
    color.g = Convert4To8(bitfieldExtract(val, 4, 4));
    color.b = Convert4To8(bitfieldExtract(val, 0, 4));
  }
#else
  // Not used.
  color = uint4(0, 0, 0, 0);
#endif

  return color;
}

float4 GetPaletteColorNormalized(uint index)
{
  uint4 color = GetPaletteColor(index);
  return float4(color) / 255.0;
}
#endif // defined(HAS_PALETTE)

)";

static const std::map<TextureFormat, DecodingShaderInfo> s_decoding_shader_info{
    {TextureFormat::I4,
     {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x8 blocks, 4 bits per pixel
        // We need to do the tiling manually here because the texel size is smaller than
        // the size of the buffer elements.
        uint2 block = coords.xy / 8u;
        uint2 offset = coords.xy % 8u;
        uint buffer_pos = 0u;
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x / 2u;

        // Select high nibble for odd texels, low for even.
        uint val = FETCH(buffer_pos);
        uint i;
        if ((coords.x & 1u) == 0u)
          i = Convert4To8((val >> 4));
        else
          i = Convert4To8((val & 0x0Fu));

        uint4 color = uint4(i, i, i, i);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )"}},
    {TextureFormat::IA4,
     {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint val = FETCH(buffer_pos);
        uint i = Convert4To8((val & 0x0Fu));
        uint a = Convert4To8((val >> 4));
        uint4 color = uint4(i, i, i, a);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},
    {TextureFormat::I8,
     {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint i = FETCH(buffer_pos);
        uint4 color = uint4(i, i, i, i);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},
    {TextureFormat::IA8,
     {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks, 16 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = FETCH(buffer_pos);
        uint a = (val & 0xFFu);
        uint i = (val >> 8);
        uint4 color = uint4(i, i, i, a);
        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},
    {TextureFormat::RGB565,
     {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = Swap16(FETCH(buffer_pos));

        uint4 color;
        color.x = Convert5To8(bitfieldExtract(val, 11, 5));
        color.y = Convert6To8(bitfieldExtract(val, 5, 6));
        color.z = Convert5To8(bitfieldExtract(val, 0, 5));
        color.a = 255u;

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )"}},
    {TextureFormat::RGB5A3,
     {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = Swap16(FETCH(buffer_pos));

        uint4 color;
        if ((val & 0x8000u) != 0u)
        {
          color.x = Convert5To8(bitfieldExtract(val, 10, 5));
          color.y = Convert5To8(bitfieldExtract(val, 5, 5));
          color.z = Convert5To8(bitfieldExtract(val, 0, 5));
          color.a = 255u;
        }
        else
        {
          color.a = Convert3To8(bitfieldExtract(val, 12, 3));
          color.r = Convert4To8(bitfieldExtract(val, 8, 4));
          color.g = Convert4To8(bitfieldExtract(val, 4, 4));
          color.b = Convert4To8(bitfieldExtract(val, 0, 4));
        }

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )"}},
    {TextureFormat::RGBA8,
     {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        // We can't use the normal calculation function, as these are packed as the AR channels
        // for the entire block, then the GB channels afterwards.
        uint2 block = coords.xy / 4u;
        uint2 offset = coords.xy % 4u;
        uint buffer_pos = 0u;

        // Our buffer has 16-bit elements, so the offsets here are half what they would be in bytes.
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x;

        // The two GB channels follow after the block's AR channels.
        uint val1 = FETCH(buffer_pos +  0u);
        uint val2 = FETCH(buffer_pos + 16u);

        uint4 color;
        color.a = (val1 & 0xFFu);
        color.r = (val1 >> 8);
        color.g = (val2 & 0xFFu);
        color.b = (val2 >> 8);

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},
    {TextureFormat::CMPR,
     {TEXEL_BUFFER_FORMAT_R32G32_UINT, 0, 64, 1, true,
      R"(
      // In the compute version of this decoder, we flatten the blocks to a one-dimension array.
      // Each group is subdivided into 16, and the first thread in each group fetches the DXT data.
      // All threads then calculate the possible colors for the block and write to the output image.

      #define GROUP_SIZE 64u
      #define BLOCK_SIZE_X 4u
      #define BLOCK_SIZE_Y 4u
      #define BLOCK_SIZE (BLOCK_SIZE_X * BLOCK_SIZE_Y)
      #define BLOCKS_PER_GROUP (GROUP_SIZE / BLOCK_SIZE)

      uint DXTBlend(uint v1, uint v2)
      {
        // 3/8 blend, which is close to 1/3
        return ((v1 * 3u + v2 * 5u) >> 3);
      }

      GROUP_SHARED uint2 shared_temp[BLOCKS_PER_GROUP];

      DEFINE_MAIN(GROUP_SIZE, 1)
      {
        uint local_thread_id = gl_LocalInvocationID.x;
        uint block_in_group = local_thread_id / BLOCK_SIZE;
        uint thread_in_block = local_thread_id % BLOCK_SIZE;
        uint block_index = gl_WorkGroupID.x * BLOCKS_PER_GROUP + block_in_group;

        // Annoyingly, we can't precalculate this as a uniform because the DXT block size differs
        // from the block size of the overall texture (4 vs 8). We can however use a multiply and
        // subtraction to avoid the modulo for calculating the block's X coordinate.
        uint blocks_wide = u_src_size.x / BLOCK_SIZE_X;
        uint2 block_coords;
        block_coords.y = block_index / blocks_wide;
        block_coords.x = block_index - (block_coords.y * blocks_wide);

        // Only the first thread for each block reads from the texel buffer.
        if (thread_in_block == 0u)
        {
          // Calculate tiled block coordinates.
          uint2 tile_block_coords = block_coords / 2u;
          uint2 subtile_block_coords = block_coords % 2u;
          uint buffer_pos = 0u;
          buffer_pos += tile_block_coords.y * u_src_row_stride;
          buffer_pos += tile_block_coords.x * 4u;
          buffer_pos += subtile_block_coords.y * 2u;
          buffer_pos += subtile_block_coords.x;

          // Read the entire DXT block to shared memory.
          uint2 raw_data = FETCH(buffer_pos);
          shared_temp[block_in_group] = raw_data;
        }

        // Ensure store is completed before the remaining threads in the block continue.
        GROUP_MEMORY_BARRIER_WITH_SYNC;

        // Unpack colors and swap BE to LE.
        uint2 raw_data = shared_temp[block_in_group];
        uint swapped = ((raw_data.x & 0xFF00FF00u) >> 8) | ((raw_data.x & 0x00FF00FFu) << 8);
        uint c1 = swapped & 0xFFFFu;
        uint c2 = swapped >> 16;

        // Expand 5/6 bit channels to 8-bits per channel.
        uint blue1 = Convert5To8(bitfieldExtract(c1, 0, 5));
        uint blue2 = Convert5To8(bitfieldExtract(c2, 0, 5));
        uint green1 = Convert6To8(bitfieldExtract(c1, 5, 6));
        uint green2 = Convert6To8(bitfieldExtract(c2, 5, 6));
        uint red1 = Convert5To8(bitfieldExtract(c1, 11, 5));
        uint red2 = Convert5To8(bitfieldExtract(c2, 11, 5));

        // Determine the four colors the block can use.
        // It's quicker to just precalculate all four colors rather than branching on the index.
        // NOTE: These must be masked with 0xFF. This is done at the normalization stage below.
        uint4 color0, color1, color2, color3;
        color0 = uint4(red1, green1, blue1, 255u);
        color1 = uint4(red2, green2, blue2, 255u);
        if (c1 > c2)
        {
          color2 = uint4(DXTBlend(red2, red1), DXTBlend(green2, green1), DXTBlend(blue2, blue1), 255u);
          color3 = uint4(DXTBlend(red1, red2), DXTBlend(green1, green2), DXTBlend(blue1, blue2), 255u);
        }
        else
        {
          color2 = uint4((red1 + red2) / 2u, (green1 + green2) / 2u, (blue1 + blue2) / 2u, 255u);
          color3 = uint4((red1 + red2) / 2u, (green1 + green2) / 2u, (blue1 + blue2) / 2u, 0u);
        }

        // Calculate the texel coordinates that we will write to.
        // The divides/modulo here should be turned into a shift/binary AND.
        uint local_y = thread_in_block / BLOCK_SIZE_X;
        uint local_x = thread_in_block % BLOCK_SIZE_X;
        uint global_x = block_coords.x * BLOCK_SIZE_X + local_x;
        uint global_y = block_coords.y * BLOCK_SIZE_Y + local_y;

        // Use the coordinates within the block to shift the 32-bit value containing
        // all 16 indices to a single 2-bit index.
        uint index = bitfieldExtract(raw_data.y, int((local_y * 8u) + (6u - local_x * 2u)), 2);

        // Select the un-normalized color from the precalculated color array.
        // Using a switch statement here removes the need for dynamic indexing of an array.
        uint4 color;
        switch (index)
        {
        case 0u:  color = color0;   break;
        case 1u:  color = color1;   break;
        case 2u:  color = color2;   break;
        case 3u:  color = color3;   break;
        default:  color = color0;   break;
        }

        // Normalize and write to the output image.
        float4 norm_color = float4(color & 0xFFu) / 255.0;
        imageStore(output_image, int3(int2(uint2(global_x, global_y)), 0), norm_color);
      }
      )"}},
    {TextureFormat::C4,
     {TEXEL_BUFFER_FORMAT_R8_UINT, static_cast<u32>(TexDecoder_GetPaletteSize(TextureFormat::C4)),
      8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x8 blocks, 4 bits per pixel
        // We need to do the tiling manually here because the texel size is smaller than
        // the size of the buffer elements.
        uint2 block = coords.xy / 8u;
        uint2 offset = coords.xy % 8u;
        uint buffer_pos = 0u;
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x / 2u;

        // Select high nibble for odd texels, low for even.
        uint val = FETCH(buffer_pos);
        uint index = ((coords.x & 1u) == 0u) ? (val >> 4) : (val & 0x0Fu);
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )"}},
    {TextureFormat::C8,
     {TEXEL_BUFFER_FORMAT_R8_UINT, static_cast<u32>(TexDecoder_GetPaletteSize(TextureFormat::C8)),
      8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint index = FETCH(buffer_pos);
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},
    {TextureFormat::C14X2,
     {TEXEL_BUFFER_FORMAT_R16_UINT,
      static_cast<u32>(TexDecoder_GetPaletteSize(TextureFormat::C14X2)), 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks, 16 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint index = Swap16(FETCH(buffer_pos)) & 0x3FFFu;
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )"}},

    // We do the inverse BT.601 conversion for YCbCr to RGB
    // http://www.equasys.de/colorconversion.html#YCbCr-RGBColorFormatConversion
    // TODO: Use more precise numbers for this conversion (although on real hardware, the XFB isn't
    // in a real texture format, so does this conversion actually ever happen?)
    {TextureFormat::XFB,
     {TEXEL_BUFFER_FORMAT_RGBA8_UINT, 0, 8, 8, false,
      R"(
      DEFINE_MAIN(8, 8)
      {
        uint2 uv = gl_GlobalInvocationID.xy;
        uint buffer_pos = (uv.y * u_src_row_stride) + (uv.x / 2u);
        float4 yuyv = float4(FETCH(buffer_pos));

        float y = (uv.x & 1u) != 0u ? yuyv.b : yuyv.r;

        float yComp = 1.164 * (y - 16.0);
        float uComp = yuyv.g - 128.0;
        float vComp = yuyv.a - 128.0;

        float4 rgb = float4(yComp + (1.596 * vComp),
                        yComp - (0.813 * vComp) - (0.391 * uComp),
                        yComp + (2.018 * uComp),
                        255.0);
        float4 rgba_norm = rgb / 255.0;
        imageStore(output_image, int3(int2(uv), 0), rgba_norm);
      }
      )"}}};

const DecodingShaderInfo* GetDecodingShaderInfo(TextureFormat format)
{
  auto iter = s_decoding_shader_info.find(format);
  return iter != s_decoding_shader_info.end() ? &iter->second : nullptr;
}

std::pair<u32, u32> GetDispatchCount(const DecodingShaderInfo* info, u32 width, u32 height)
{
  // Flatten to a single dimension?
  if (info->group_flatten)
    return {(width * height + (info->group_size_x - 1)) / info->group_size_x, 1};

  return {(width + (info->group_size_x - 1)) / info->group_size_x,
          (height + (info->group_size_y - 1)) / info->group_size_y};
}

ShaderCode GenerateDecodingShader(TextureFormat format, std::optional<TLUTFormat> palette_format,
                                   APIType api_type)
{
  const DecodingShaderInfo* info = GetDecodingShaderInfo(format);
  ShaderCode ss;
  if (!info)
    return ss;

  if (palette_format.has_value())
  {
    switch (*palette_format)
    {
    case TLUTFormat::IA8:
      ss.Puts("#define PALETTE_FORMAT_IA8 1\n");
      break;
    case TLUTFormat::RGB565:
      ss.Puts("#define PALETTE_FORMAT_RGB565 1\n");
      break;
    case TLUTFormat::RGB5A3:
      ss.Puts("#define PALETTE_FORMAT_RGB5A3 1\n");
      break;
    }
  }

  switch (info->buffer_format)
  {
  case TEXEL_BUFFER_FORMAT_R8_UINT:
    ss.Puts("#define TEXEL_BUFFER_FORMAT_R8 1\n");
    break;
  case TEXEL_BUFFER_FORMAT_R16_UINT:
    ss.Puts("#define TEXEL_BUFFER_FORMAT_R16 1\n");
    break;
  case TEXEL_BUFFER_FORMAT_RGBA8_UINT:
    ss.Puts("#define TEXEL_BUFFER_FORMAT_RGBA8 1\n");
    break;
  case TEXEL_BUFFER_FORMAT_R32G32_UINT:
    ss.Puts("#define TEXEL_BUFFER_FORMAT_R32G32 1\n");
    break;
  case NUM_TEXEL_BUFFER_FORMATS:
    ASSERT(false);
    break;
  }

  ss.Puts(decoding_shader_header);
  ss.Puts(info->shader_body);

  return ss;
}

ShaderCode GeneratePaletteConversionShader(TLUTFormat palette_format, APIType api_type)
{
  ShaderCode ss;

  ss.Puts(R"(
int Convert3To8(int v)
{
  // Swizzle bits: 00000123 -> 12312312
  return (v << 5) | (v << 2) | (v >> 1);
}
int Convert4To8(int v)
{
  // Swizzle bits: 00001234 -> 12341234
  return (v << 4) | v;
}
int Convert5To8(int v)
{
  // Swizzle bits: 00012345 -> 12345123
  return (v << 3) | (v >> 2);
}
int Convert6To8(int v)
{
  // Swizzle bits: 00123456 -> 12345612
  return (v << 2) | (v >> 4);
})");

  switch (palette_format)
  {
  case TLUTFormat::IA8:
    ss.Puts(R"(
float4 DecodePixel(int val)
{
  int i = val & 0xFF;
  int a = val >> 8;
  return float4(i, i, i, a) / 255.0;
})");
    break;

  case TLUTFormat::RGB565:
    ss.Puts(R"(
float4 DecodePixel(int val)
{
  int r, g, b, a;
  r = Convert5To8((val >> 11) & 0x1f);
  g = Convert6To8((val >> 5) & 0x3f);
  b = Convert5To8((val) & 0x1f);
  a = 0xFF;
  return float4(r, g, b, a) / 255.0;
})");
    break;

  case TLUTFormat::RGB5A3:
    ss.Puts(R"(
float4 DecodePixel(int val)
{
  int r,g,b,a;
  if ((val&0x8000) > 0)
  {
    r=Convert5To8((val>>10) & 0x1f);
    g=Convert5To8((val>>5 ) & 0x1f);
    b=Convert5To8((val    ) & 0x1f);
    a=0xFF;
  }
  else
  {
    a=Convert3To8((val>>12) & 0x7);
    r=Convert4To8((val>>8 ) & 0xf);
    g=Convert4To8((val>>4 ) & 0xf);
    b=Convert4To8((val    ) & 0xf);
  }
  return float4(r, g, b, a) / 255.0;
})");
    break;

  default:
    PanicAlert("Unknown format");
    break;
  }

  ss.Puts("\n");

  if (api_type == APIType::Metal)
    ss.Puts("SSBO_BINDING(0) readonly buffer Palette { uint palette[]; };\n");
  else
    ss.Puts("TEXEL_BUFFER_BINDING(0) uniform usamplerBuffer samp0;\n");
  ss.Puts("SAMPLER_BINDING(1) uniform sampler2DArray samp1;\n");
  ss.Puts("UBO_BINDING(std140, 1) uniform PSBlock {\n");

  ss.Puts("  float multiplier;\n");
  ss.Puts("  int texel_buffer_offset;\n");
  ss.Puts("};\n");

  if (g_backend_info.bSupportsGeometryShaders)
  {
    ss.Puts("VARYING_LOCATION(0) in VertexData {\n");
    ss.Puts("  float3 v_tex0;\n");
    ss.Puts("};\n");
  }
  else
  {
    ss.Puts("VARYING_LOCATION(0) in float3 v_tex0;\n");
  }
  ss.Puts("FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n");
  ss.Puts("void main() {\n");
  ss.Puts("  float3 coords = v_tex0;\n");
  ss.Puts("  int src = int(round(texture(samp1, coords).r * multiplier));\n");
  if (api_type == APIType::Metal)
    ss.Puts("  src = int((palette[uint(src) >> 1] >> ((uint(src) & 1u) << 4)) & 0xFFFFu);\n");
  else
    ss.Puts("  src = int(texelFetch(samp0, src + texel_buffer_offset).r);\n");

  ss.Puts("  src = ((src << 8) | (src >> 8)) & 0xFFFF;\n");
  ss.Puts("  ocol0 = DecodePixel(src);\n");
  ss.Puts("}\n");

  return ss;
}

}  // namespace TextureConversionShaderTiled
