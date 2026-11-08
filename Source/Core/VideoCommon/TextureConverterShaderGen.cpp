// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureConverterShaderGen.h"

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
              "the pixel shader's text spells them out");

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

static void WriteHeader(APIType api_type, ShaderCode& out)
{
  out.Puts("UBO_BINDING(std140, 1) uniform PSBlock {\n"
            "  float2 src_offset, src_size;\n"
            "  uint3 filter_coefficients;\n"
            "  float gamma_rcp;\n"
            "  float2 clamp_tb;\n"
            "  float pixel_height;\n"
            "  uint copy_flags;\n"
            "};\n");
}

ShaderCode GenerateVertexShader(APIType api_type)
{
  ShaderCode out;
  WriteHeader(api_type, out);

  if (g_backend_info.bSupportsGeometryShaders)
  {
    out.Puts("VARYING_LOCATION(0) out VertexData {\n"
              "  float3 v_tex0;\n"
              "};\n");
  }
  else
  {
    out.Puts("VARYING_LOCATION(0) out float3 v_tex0;\n");
  }
  out.Puts("#define id gl_VertexID\n"
            "#define opos gl_Position\n"
            "void main() {\n");
  out.Puts("  v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);\n");
  out.Puts("  opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n");
  out.Puts("  v_tex0 = float3(src_offset + (src_size * v_tex0.xy), 0.0f);\n");

  // NDC space is flipped in Vulkan
  if (api_type == APIType::Vulkan)
    out.Puts("  opos.y = -opos.y;\n");

  out.Puts("}\n");

  return out;
}

/* The output of one format, as a shader built for it alone writes it. */
static void WriteFormatBody(ShaderCode& out, EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:  // R4
    out.Puts("  float red = float(texcol_raw.r & 0xF0u) / 240.0;\n"
             "  ocol0 = float4(red, red, red, red);\n");
    break;

  case EFBCopyFormat::R8_0x1:  // R8
  case EFBCopyFormat::R8:      // R8
    out.Puts("  ocol0 = float4(texcol_raw).rrrr / 255.0;\n");
    break;

  case EFBCopyFormat::RA4:  // RA4
    out.Puts("  float2 red_alpha = float2(texcol_raw.ra & 0xF0u) / 240.0;\n"
             "  ocol0 = red_alpha.rrrg;\n");
    break;

  case EFBCopyFormat::RA8:  // RA8
    out.Puts("  ocol0 = float4(texcol_raw).rrra / 255.0;\n");
    break;

  case EFBCopyFormat::A8:  // A8
    out.Puts("  ocol0 = float4(texcol_raw).aaaa / 255.0;\n");
    break;

  case EFBCopyFormat::G8:  // G8
    out.Puts("  ocol0 = float4(texcol_raw).gggg / 255.0;\n");
    break;

  case EFBCopyFormat::B8:  // B8
    out.Puts("  ocol0 = float4(texcol_raw).bbbb / 255.0;\n");
    break;

  case EFBCopyFormat::RG8:  // RG8
    out.Puts("  ocol0 = float4(texcol_raw).rrrg / 255.0;\n");
    break;

  case EFBCopyFormat::GB8:  // GB8
    out.Puts("  ocol0 = float4(texcol_raw).gggb / 255.0;\n");
    break;

  case EFBCopyFormat::RGB565:  // RGB565
    out.Puts("  float2 red_blue = float2(texcol_raw.rb & 0xF8u) / 248.0;\n"
             "  float green = float(texcol_raw.g & 0xFCu) / 252.0;\n"
             "  ocol0 = float4(red_blue.r, green, red_blue.g, 1.0);\n");
    break;

  case EFBCopyFormat::RGB5A3:  // RGB5A3
    // TODO: The MSB controls whether we have RGB5 or RGB4A3, this selection
    // will need to be implemented once we move away from floats.
    out.Puts("  float3 color = float3(texcol_raw.rgb & 0xF8u) / 248.0;\n"
             "  float alpha = float(texcol_raw.a & 0xE0u) / 224.0;\n"
             "  ocol0 = float4(color, alpha);\n");
    break;

  case EFBCopyFormat::XFB:
    out.Puts("  ocol0 = float4(float3(texcol_raw.rgb) / 255.0, 1.0);\n");
    break;

  case EFBCopyFormat::RGBA8:  // RGBA8
  default:
    out.Puts("  ocol0 = float4(texcol_raw.rgba) / 255.0;\n");
    break;
  }
}

ShaderCode GeneratePixelShader(APIType api_type, const EFBCopyFormat* format)
{
  ShaderCode out;
  WriteHeader(api_type, out);

  out.Puts("SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n");
  // A depth copy reads the first layer alone, with stereo's mono depth.
  out.Printf("uint4 SampleEFB(float3 uv, float y_offset) {\n"
             "  float4 tex_sample = texture(samp0, float3(uv.x, clamp(uv.y + (y_offset * "
             "pixel_height), clamp_tb.x, clamp_tb.y), %s));\n",
             g_ActiveConfig.bStereoEFBMonoDepth ? "(copy_flags & 16u) != 0u ? 0.0 : uv.z" :
                                                  "uv.z");
  out.Puts("  if ((copy_flags & 16u) != 0u) {\n");
  if (!g_backend_info.bSupportsReversedDepthRange)
    out.Puts("    tex_sample.x = 1.0 - tex_sample.x;\n");
  out.Puts("    uint depth = uint(tex_sample.x * 16777216.0);\n"
           "    return uint4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);\n"
           "  }\n"
           "  return uint4(tex_sample * 255.0);\n"
           "}\n");

  if (g_backend_info.bSupportsGeometryShaders)
  {
    out.Puts("VARYING_LOCATION(0) in VertexData {\n"
             "  float3 v_tex0;\n"
             "};\n");
  }
  else
  {
    out.Puts("VARYING_LOCATION(0) in vec3 v_tex0;\n");
  }

  out.Puts("FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;\n"
           "void main()\n"
           "{\n");

  // The copy filter applies to both color and depth copies. This has been verified on hardware.
  // The filter is only applied to the RGB channels, the alpha channel is left intact.
  out.Puts("  uint4 current_row = SampleEFB(v_tex0, 0.0f);\n"
           "  uint3 combined_rows;\n"
           "  if ((copy_flags & 1u) != 0u) {\n"
           "    uint4 prev_row = SampleEFB(v_tex0, -1.0f);\n"
           "    uint4 next_row = SampleEFB(v_tex0, 1.0f);\n"
           "    combined_rows = prev_row.rgb * filter_coefficients[0] +\n"
           "                    current_row.rgb * filter_coefficients[1] +\n"
           "                    next_row.rgb * filter_coefficients[2];\n"
           "  } else {\n"
           "    combined_rows = current_row.rgb * filter_coefficients[1];\n"
           "  }\n"
           "  // Shift right by 6 to divide by 64, as filter coefficients\n"
           "  // that sum to 64 result in no change in brightness\n"
           "  uint4 texcol_raw = uint4(combined_rows.rgb >> 6,\n"
           "                           (copy_flags & 2u) != 0u ? current_row.a : 255u);\n"
           "  if ((copy_flags & 4u) != 0u)\n"
           "    texcol_raw &= 0x1ffu;\n");
  // Note that overflow occurs when the sum of values is >= 128, but this max situation can be hit
  // on >= 64, so we always include it.
  out.Puts("  texcol_raw = min(texcol_raw, uint4(255, 255, 255, 255));\n"
           "  if ((copy_flags & 8u) != 0u)\n"
           "    texcol_raw = uint4(round(pow(abs(float4(texcol_raw) / 255.0),\n"
           "                       float4(gamma_rcp, gamma_rcp, gamma_rcp, 1.0)) * 255.0));\n");

  out.Puts("  if ((copy_flags & 32u) != 0u) {\n"
           "    // Intensity/YUV format conversion constants determined by hardware testing\n"
           "    const float4 y_const = float4( 66, 129,  25,  16);\n"
           "    const float4 u_const = float4(-38, -74, 112, 128);\n"
           "    const float4 v_const = float4(112, -94, -18, 128);\n"
           "    // Intensity/YUV format conversion\n"
           "    texcol_raw.rgb = uint3(dot(y_const, float4(texcol_raw.rgb, 256)),\n"
           "                           dot(u_const, float4(texcol_raw.rgb, 256)),\n"
           "                           dot(v_const, float4(texcol_raw.rgb, 256)));\n"
           "    // Divide by 256 and round .5 and higher up\n"
           "    texcol_raw.rgb = (texcol_raw.rgb >> 8) + ((texcol_raw.rgb >> 7) & 1u);\n"
           "  }\n");

  if (format)
  {
    WriteFormatBody(out, *format);
    out.Puts("}\n");
    return out;
  }

  // The format: each channel is a component of texcol_raw, masked to the bits the format keeps
  // and scaled by the mask, which is the largest value they hold; or 1.
  out.Puts("  uint4 source = (uint4(copy_flags) >> uint4(8u, 10u, 12u, 14u)) & 3u;\n"
           "  uint4 picked = uint4(source.x == 0u ? texcol_raw.r : source.x == 1u ? texcol_raw.g :\n"
           "                       source.x == 2u ? texcol_raw.b : texcol_raw.a,\n"
           "                       source.y == 0u ? texcol_raw.r : source.y == 1u ? texcol_raw.g :\n"
           "                       source.y == 2u ? texcol_raw.b : texcol_raw.a,\n"
           "                       source.z == 0u ? texcol_raw.r : source.z == 1u ? texcol_raw.g :\n"
           "                       source.z == 2u ? texcol_raw.b : texcol_raw.a,\n"
           "                       source.w == 0u ? texcol_raw.r : source.w == 1u ? texcol_raw.g :\n"
           "                       source.w == 2u ? texcol_raw.b : texcol_raw.a);\n"
           "  uint4 mask = (uint4(copy_flags) >> uint4(16u, 19u, 22u, 25u)) & 7u;\n"
           "  mask = uint4(mask.x == 1u ? 0xF0u : mask.x == 2u ? 0xF8u : mask.x == 3u ? 0xFCu :\n"
           "               mask.x == 4u ? 0xE0u : 0xFFu,\n"
           "               mask.y == 1u ? 0xF0u : mask.y == 2u ? 0xF8u : mask.y == 3u ? 0xFCu :\n"
           "               mask.y == 4u ? 0xE0u : 0xFFu,\n"
           "               mask.z == 1u ? 0xF0u : mask.z == 2u ? 0xF8u : mask.z == 3u ? 0xFCu :\n"
           "               mask.z == 4u ? 0xE0u : 0xFFu,\n"
           "               mask.w == 1u ? 0xF0u : mask.w == 2u ? 0xF8u : mask.w == 3u ? 0xFCu :\n"
           "               mask.w == 4u ? 0xE0u : 0xFFu);\n"
           "  float4 one = float4((uint4(copy_flags) >> uint4(28u, 29u, 30u, 31u)) & 1u);\n"
           "  ocol0 = mix(float4(picked & mask) / float4(mask), float4(1.0, 1.0, 1.0, 1.0), one);\n"
           "}\n");

  return out;
}

}  // namespace TextureConversionShaderGen
