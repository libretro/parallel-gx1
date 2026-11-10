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

void EmitSamplerDeclarations(ShaderCode& code, u32 start = 0, u32 end = 1,
                             bool multisampled = false)
{
  switch (GetAPIType())
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
  {
    const char* array_type = multisampled ? "sampler2DMSArray" : "sampler2DArray";

    for (u32 i = start; i < end; i++)
    {
      code.Printf("SAMPLER_BINDING(%u) uniform %s samp%u;\n", i, array_type, i);
    }
  }
  break;
  default:
    break;
  }
}

void EmitSampleTexture(ShaderCode& code, u32 n, const char* coords)
{
  switch (GetAPIType())
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
    code.Printf("texture(samp%u, %s)", n, coords);
    break;

  default:
    break;
  }
}

// Emits a texel fetch/load instruction. Assumes that "coords" is a 4-element vector, with z
// containing the layer, and w containing the mipmap level.
void EmitTextureLoad(ShaderCode& code, u32 n, const char* coords)
{
  switch (GetAPIType())
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
    code.Printf("texelFetch(samp%u, (%s).xyz, (%s).w)", n, coords, coords);
    break;

  default:
    break;
  }
}

void EmitPixelMainDeclaration(ShaderCode& code, u32 num_tex_inputs, u32 num_color_inputs,
                              const char* output_type = "float4",
                              const char* extra_vars = "", bool emit_frag_coord = false)
{
  switch (GetAPIType())
  {
  case APIType::D3D:
  case APIType::Metal:
  case APIType::OpenGL:
  case APIType::Vulkan:
  {
    if (g_backend_info.bSupportsGeometryShaders)
    {
      code.Puts("VARYING_LOCATION(0) in VertexData {\n");
      for (u32 i = 0; i < num_tex_inputs; i++)
        code.Printf("  float3 v_tex%u;\n", i);
      for (u32 i = 0; i < num_color_inputs; i++)
        code.Printf("  float4 v_col%u;\n", i);
      code.Puts("};\n");
    }
    else
    {
      for (u32 i = 0; i < num_tex_inputs; i++)
        code.Printf("VARYING_LOCATION(%u) in float3 v_tex%u;\n", i, i);
      for (u32 i = 0; i < num_color_inputs; i++)
        code.Printf("VARYING_LOCATION(%u) in float4 v_col%u;\n", num_tex_inputs + i, i);
    }

    code.Printf("FRAGMENT_OUTPUT_LOCATION(0) out %s ocol0;\n", output_type);
    code.Printf("%s\n", extra_vars);
    if (emit_frag_coord)
      code.Puts("#define frag_coord gl_FragCoord\n");
    code.Puts("void main()\n");
  }
  break;

  default:
    break;
  }
}
}  // Anonymous namespace

ShaderCode GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples)
{
  ShaderCode code;
  EmitSamplerDeclarations(code, 0, 1, samples > 1);
  EmitPixelMainDeclaration(code, 1, 0, "float4",

                           "");
  code.Puts("{\n"
            "  int layer = int(v_tex0.z);\n");
  code.Puts("  int3 coords = int3(int2(gl_FragCoord.xy), layer);\n");

  if (samples == 1)
  {
    // No MSAA at all.
    code.Puts("  float4 val = texelFetch(samp0, coords, 0);\n");
  }
  else if (g_ActiveConfig.bSSAA)
  {
    // Sample shading, shader runs once per sample
    code.Puts("  float4 val = texelFetch(samp0, coords, gl_SampleID);");
  }
  else
  {
    // MSAA without sample shading, average out all samples.
    code.Puts("  float4 val = float4(0.0f, 0.0f, 0.0f, 0.0f);\n");
    code.Printf("  for (int i = 0; i < %u; i++)\n", samples);
    code.Puts("    val += texelFetch(samp0, coords, i);\n");
    code.Printf("  val /= float(%u);\n", samples);
  }

  switch (convtype)
  {
  case EFBReinterpretType::RGB8ToRGBA6:
    code.Puts("  int4 src8 = int4(round(val * 255.f));\n"
              "  int4 dst6;\n"
              "  dst6.r = src8.r >> 2;\n"
              "  dst6.g = ((src8.r & 0x3) << 4) | (src8.g >> 4);\n"
              "  dst6.b = ((src8.g & 0xF) << 2) | (src8.b >> 6);\n"
              "  dst6.a = src8.b & 0x3F;\n"
              "  ocol0 = float4(dst6) / 63.f;\n");
    break;

  case EFBReinterpretType::RGB8ToRGB565:
    code.Puts("  ocol0 = val;\n");
    break;

  case EFBReinterpretType::RGBA6ToRGB8:
    code.Puts("  int4 src6 = int4(round(val * 63.f));\n"
              "  int4 dst8;\n"
              "  dst8.r = (src6.r << 2) | (src6.g >> 4);\n"
              "  dst8.g = ((src6.g & 0xF) << 4) | (src6.b >> 2);\n"
              "  dst8.b = ((src6.b & 0x3) << 6) | src6.a;\n"
              "  dst8.a = 255;\n"
              "  ocol0 = float4(dst8) / 255.f;\n");
    break;

  case EFBReinterpretType::RGBA6ToRGB565:
    code.Puts("  ocol0 = val;\n");
    break;

  case EFBReinterpretType::RGB565ToRGB8:
    code.Puts("  ocol0 = val;\n");
    break;

  case EFBReinterpretType::RGB565ToRGBA6:
    //
    code.Puts("  ocol0 = val;\n");
    break;
  }

  code.Puts("}\n");
  return code;
}

ShaderCode GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format)
{
  ShaderCode code;
  EmitSamplerDeclarations(code, 0, 1, false);
  EmitPixelMainDeclaration(code, 1, 0, "float4", "", true);
  code.Puts("{\n"
            "  int layer = int(v_tex0.z);\n"
            "  int4 coords = int4(int2(frag_coord.xy), layer, 0);\n");

  // Convert to a 32-bit value encompassing all channels, filling the most significant bits with
  // zeroes.
  code.Puts("  uint raw_value;\n");
  switch (from_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n"
              "  raw_value = uint(temp_value.r * 255.0);\n");
  }
  break;

  case TextureFormat::IA8:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n"
              "  raw_value = uint(temp_value.r * 255.0) | (uint(temp_value.a * 255.0) << 8);\n");
  }
  break;

  case TextureFormat::I4:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n"
              "  raw_value = uint(temp_value.r * 15.0);\n");
  }
  break;

  case TextureFormat::IA4:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n"
              "  raw_value = uint(temp_value.r * 15.0) | (uint(temp_value.a * 15.0) << 4);\n");
  }
  break;

  case TextureFormat::RGB565:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n"
              "  raw_value = uint(temp_value.b * 31.0) | (uint(temp_value.g * 63.0) << 5) |\n"
              "              (uint(temp_value.r * 31.0) << 11);\n");
  }
  break;

  case TextureFormat::RGB5A3:
  {
    code.Puts("  float4 temp_value = ");
    EmitTextureLoad(code, 0, "coords");
    code.Puts(";\n");

    // 0.8784 = 224 / 255 which is the maximum alpha value that can be represented in 3 bits
    code.Puts("  if (temp_value.a > 0.878f) {\n"
              "    raw_value = (uint(temp_value.b * 31.0)) | (uint(temp_value.g * 31.0) << 5) |\n"
              "                (uint(temp_value.r * 31.0) << 10) | 0x8000u;\n"
              "  } else {\n"
              "     raw_value = (uint(temp_value.b * 15.0)) | (uint(temp_value.g * 15.0) << 4) |\n"
              "                 (uint(temp_value.r * 15.0) << 8) | (uint(temp_value.a * 7.0) << 12);\n"
              "  }\n");
  }
  break;

  default:
    WARN_LOG_FMT(VIDEO, "From format {} is not supported", from_format);
  {
    ShaderCode unsupported;
    unsupported.Puts("{}\n");
    return unsupported;
  }
  }

  // Now convert it to its new representation.
  switch (to_format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
  {
    code.Puts("  float orgba = float(raw_value & 0xFFu) / 255.0;\n"
              "  ocol0 = float4(orgba, orgba, orgba, orgba);\n");
  }
  break;

  case TextureFormat::IA8:
  {
    code.Puts("  float orgb = float(raw_value & 0xFFu) / 255.0;\n"
              "  ocol0 = float4(orgb, orgb, orgb, float((raw_value >> 8) & 0xFFu) / 255.0);\n");
  }
  break;

  case TextureFormat::IA4:
  {
    code.Puts("  float orgb = float(raw_value & 0xFu) / 15.0;\n"
              "  ocol0 = float4(orgb, orgb, orgb, float((raw_value >> 4) & 0xFu) / 15.0);\n");
  }
  break;

  case TextureFormat::RGB565:
  {
    code.Puts("  ocol0 = float4(float((raw_value >> 10) & 0x1Fu) / 31.0,\n"
              "                 float((raw_value >> 5) & 0x1Fu) / 31.0,\n"
              "                 float(raw_value & 0x1Fu) / 31.0, 1.0);\n");
  }
  break;

  case TextureFormat::RGB5A3:
  {
    code.Puts("  if ((raw_value & 0x8000u) != 0u) {\n"
              "    ocol0 = float4(float((raw_value >> 10) & 0x1Fu) / 31.0,\n"
              "                   float((raw_value >> 5) & 0x1Fu) / 31.0,\n"
              "                   float(raw_value & 0x1Fu) / 31.0, 1.0);\n"
              "  } else {\n"
              "    ocol0 = float4(float((raw_value >> 8) & 0x0Fu) / 15.0,\n"
              "                   float((raw_value >> 4) & 0x0Fu) / 15.0,\n"
              "                   float(raw_value & 0x0Fu) / 15.0,\n"
              "                   float((raw_value >> 12) & 0x07u) / 7.0);\n"
              "  }\n");
  }
  break;
  default:
    WARN_LOG_FMT(VIDEO, "To format {} is not supported", to_format);
  {
    ShaderCode unsupported;
    unsupported.Puts("{}\n");
    return unsupported;
  }
  }

  code.Puts("}\n");
  return code;
}

ShaderCode GenerateEFBRestorePixelShader()
{
  ShaderCode code;
  EmitSamplerDeclarations(code, 0, 2, false);
  EmitPixelMainDeclaration(code, 1, 0, "float4", "");
  code.Puts("{\n"
            "  ocol0 = ");
  EmitSampleTexture(code, 0, "v_tex0");
  code.Puts(";\n");
  code.Puts("  gl_FragDepth = ");
  EmitSampleTexture(code, 1, "v_tex0");
  code.Puts(".r;\n"
            "}\n");
  return code;
}

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

/* what EmitVertexMainDeclaration declared, without texture inputs */
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

struct rslc_var* fb_sampler(struct shader_build* sb, bool multisampled)
{
  struct rslc_b_quals q;
  rslc_b_quals_init(&q);
  sb_sampler(sb, &q, 0);
  return rslc_b_declare(sb->b, &q, "samp0",
                        rslc_b_sampler_type(sb->b,
                                            multisampled ? RSLC_B_SAMPLER_2D_MS_ARRAY :
                                                           RSLC_B_SAMPLER_2D_ARRAY,
                                            RSLC_BASE_FLOAT),
                        0);
}

/* what EmitPixelMainDeclaration declared */
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

}  // namespace FramebufferShaderGen

