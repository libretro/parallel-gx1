// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderGen.h"

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

static void WritePrimitiveExpand(APIType api_type, const ShaderHostConfig& host_config,
                                 const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  if (uid_data->vs_expand == VSExpand::None)
    return;

  out.Puts("InputData dolphin_primitive_expand_data(int index_offset)\n");
  out.Puts("{\n");
  if (api_type == APIType::D3D)
  {
    // D3D doesn't include the base vertex in SV_VertexID
    // See comment in UberShaderVertex for details
    out.Puts("\tuint vertex_id = (gl_VertexID >> 2) + base_vertex;\n");
  }
  else
  {
    out.Puts("\tuint vertex_id = uint(gl_VertexID) >> 2u;\n");
  }
  out.Puts("\treturn input_buffer[vertex_id + index_offset];\n");
  out.Puts("}\n"
            "\n");
}

static void WriteTransformMatrices(APIType api_type, const ShaderHostConfig& host_config,
                                   const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Puts("mat3x4 dolphin_position_matrix()\n");
  out.Puts("{\n");
  out.Puts("\tmat3x4 result;\n");
  if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
  {
    if (uid_data->vs_expand != VSExpand::None)
    {
      out.Puts("\tInputData i = dolphin_primitive_expand_data(0);\n");
      out.Puts("\tuvec4 posmtx = unpack_ubyte4(i.posmtx);\n");
    }
    // Vertex format has a per-vertex matrix
    out.Puts("\tint posidx = int(posmtx.r);\n"
              "\tresult[0] = "
              I_TRANSFORMMATRICES
              "[posidx];\n"
              "\tresult[1] = "
              I_TRANSFORMMATRICES
              "[posidx + 1];\n"
              "\tresult[2] = "
              I_TRANSFORMMATRICES
              "[posidx + 2];\n");
  }
  else
  {
    // One shared matrix
    out.Puts("\tresult[0] = "
              I_POSNORMALMATRIX
              "[0];\n"
              "\tresult[1] = "
              I_POSNORMALMATRIX
              "[1];\n"
              "\tresult[2] = "
              I_POSNORMALMATRIX
              "[2];\n");
  }
  out.Puts("\treturn result;\n");
  out.Puts("}\n"
            "\n");

  // The scale of the transform matrix is used to control the size of the emboss map effect, by
  // changing the scale of the transformed binormals (which only get used by emboss map texgens).
  // By normalising the first transformed normal (which is used by lighting calculations and needs
  // to be unit length), the same transform matrix can do double duty, scaling for emboss mapping,
  // and not scaling for lighting.
  out.Puts("mat3 dolphin_normal_matrix()\n");
  out.Puts("{\n");
  out.Puts("\tmat3 result;\n");
  if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
  {
    if (uid_data->vs_expand != VSExpand::None)
    {
      out.Puts("\tInputData i = dolphin_primitive_expand_data(0);\n");
      out.Puts("\tuvec4 posmtx = unpack_ubyte4(i.posmtx);\n");
    }
    // Vertex format has a per-vertex matrix
    out.Puts("\tint posidx = int(posmtx.r);\n");
    out.Puts("\tint normidx = posidx & 31;\n"
              "\tresult[0] = "
              I_NORMALMATRICES
              "[normidx].xyz;\n"
              "\tresult[1] = "
              I_NORMALMATRICES
              "[normidx + 1].xyz;\n"
              "\tresult[2] = "
              I_NORMALMATRICES
              "[normidx + 2].xyz;\n");
  }
  else
  {
    // One shared matrix
    out.Puts("\tresult[0] = "
              I_POSNORMALMATRIX
              "[3].xyz;\n"
              "\tresult[1] = "
              I_POSNORMALMATRIX
              "[4].xyz;\n"
              "\tresult[2] = "
              I_POSNORMALMATRIX
              "[5].xyz;\n");
  }
  out.Puts("\treturn result;\n");
  out.Puts("}\n"
            "\n");
}

static void WriteTexCoordTransforms(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];
    out.Printf("vec3 dolphin_transform_texcoord%u(vec4 coord)\n", i);
    out.Puts("{\n");
    if (!texinfo.is_regular_texgen)
    {
      out.Puts("\treturn vec3(coord.xyz);\n");
    }
    else
    {
      out.Puts("\tvec3 result;\n");
      if ((uid_data->components & (VB_HAS_TEXMTXIDX0 << i)) != 0)
      {
        out.Printf("\tint tmp = int(rawtex%u.z);\n", i);
        if (texinfo.regular.projection == TexSize::STQ)
        {
          out.Puts("\tresult = vec3(dot(coord, "
                    I_TRANSFORMMATRICES
                    "[tmp]), dot(coord, "
                    I_TRANSFORMMATRICES
                    "[tmp+1]), dot(coord, "
                    I_TRANSFORMMATRICES
                    "[tmp+2]));\n");
        }
        else
        {
          out.Puts("\tresult = vec3(dot(coord, "
                    I_TRANSFORMMATRICES
                    "[tmp]), dot(coord, "
                    I_TRANSFORMMATRICES
                    "[tmp+1]), 1);\n");
        }
      }
      else
      {
        if (texinfo.regular.projection == TexSize::STQ)
        {
          out.Printf("\tresult = vec3(dot(coord, "
                    I_TEXMATRICES
                    "[%u]), dot(coord, "
                    I_TEXMATRICES
                    "[%u]), dot(coord, "
                    I_TEXMATRICES
                    "[%u]));\n", 3 * i, 3 * i + 1, 3 * i + 2);
        }
        else
        {
          out.Printf("\tresult = vec3(dot(coord, "
                    I_TEXMATRICES
                    "[%u]), dot(coord, "
                    I_TEXMATRICES
                    "[%u]), 1);\n", 3 * i, 3 * i + 1);
        }
      }
      // CHECKME: does this only work for regular tex gen types?
      if (uid_data->dualTexTrans_enabled)
      {
        auto postmtx_index = texinfo.regular.postmtx_index;

        out.Printf("\tvec4 P0 = "
                  I_POSTTRANSFORMMATRICES
                  "[%d];\n"
                  "\tvec4 P1 = "
                  I_POSTTRANSFORMMATRICES
                  "[%d];\n"
                  "\tvec4 P2 = "
                  I_POSTTRANSFORMMATRICES
                  "[%d];\n", postmtx_index & 0x3f, (postmtx_index + 1) & 0x3f, (postmtx_index + 2) & 0x3f);

        if (texinfo.regular.postmtx_normalize)
          out.Puts("\tresult = normalize(result);\n");

        // multiply by postmatrix
        out.Puts("\tresult = vec3(dot(P0.xyz, result) + P0.w, dot(P1.xyz, result) + P1.w, dot(P2.xyz, result) + P2.w);\n");
      }

      // When q is 0, the GameCube appears to have a special case
      // This can be seen in devkitPro's neheGX Lesson08 example for Wii
      // Makes differences in Rogue Squadron 3 (Hoth sky) and The Last Story (shadow culling)
      // TODO: check if this only affects XF_TEXGEN_REGULAR
      out.Puts("\tif(result.z == 0.0f)\n"
                "\t\tresult.xy = clamp(result.xy / 2.0f, vec2(-1.0f,-1.0f), vec2(1.0f,1.0f));\n");
      out.Puts("\treturn result;\n");
    }
    out.Puts("}\n"
              "\n");
  }
}

static void WriteVertexStructs(APIType api_type, const ShaderHostConfig& host_config,
                               const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Puts("struct DolphinVertexInput\n");
  out.Puts("{\n");
  out.Puts("\tvec4 color_0;\n");
  out.Puts("\tvec4 color_1;\n");
  out.Puts("\tvec4 position;\n");
  out.Puts("\tvec3 normal;\n");
  out.Puts("\tvec3 binormal;\n");
  out.Puts("\tvec3 tangent;\n");
  for (u32 i = 0; i < 8; i++)
  {
    out.Printf("\tvec4 texture_coord_%u;\n", i);
  }
  out.Puts("};\n"
            "\n");

  out.Puts("struct DolphinVertexOutput\n");
  out.Puts("{\n");
  out.Puts("\tvec4 color_0;\n");
  out.Puts("\tvec4 color_1;\n");
  out.Puts("\tvec4 position;\n");
  out.Puts("\tvec3 normal;\n");
  for (u32 i = 0; i < 8; i++)
  {
    out.Printf("\tvec3 texture_coord_%u;\n", i);
  }
  out.Puts("};\n"
            "\n");
}

static void WriteVertexDefines(APIType, const ShaderHostConfig&,
                               const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  if ((uid_data->components & VB_HAS_COL0) != 0)
  {
    out.Puts("#define HAS_COLOR_0 1\n");
  }
  else
  {
    out.Puts("#define HAS_COLOR_0 0\n");
  }

  if ((uid_data->components & VB_HAS_COL1) != 0)
  {
    out.Puts("#define HAS_COLOR_1 1\n");
  }
  else
  {
    out.Puts("#define HAS_COLOR_1 0\n");
  }

  if ((uid_data->components & VB_HAS_NORMAL) != 0)
  {
    out.Puts("#define HAS_NORMAL 1\n");
  }
  else
  {
    out.Puts("#define HAS_NORMAL 0\n");
  }

  if ((uid_data->components & VB_HAS_BINORMAL) != 0)
  {
    out.Puts("#define HAS_BINORMAL 1\n");
  }
  else
  {
    out.Puts("#define HAS_BINORMAL 0\n");
  }

  if ((uid_data->components & VB_HAS_TANGENT) != 0)
  {
    out.Puts("#define HAS_TANGENT 1\n");
  }
  else
  {
    out.Puts("#define HAS_TANGENT 0\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; i++)
  {
    if (uid_data->texGenInfo[i].texcoord_elem_count != 0)
    {
      out.Printf("#define HAS_TEXTURE_COORD_%u 1\n", i);
    }
    else
    {
      out.Printf("#define HAS_TEXTURE_COORD_%u 0\n", i);
    }
  }

  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Printf("#define HAS_TEXTURE_COORD_%u 0\n", i);
  }
}

static void WriteEmulatedVertexBodyHeader(APIType api_type, const ShaderHostConfig& host_config,
                                          const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  static const char emulated_fragment_definition[] =
      "void dolphin_process_emulated_vertex(in DolphinVertexInput vertex_input, out "
      "DolphinVertexOutput vertex_output)";
  out.Printf("%s\n", emulated_fragment_definition);
  out.Puts("{\n");

  WriteVertexBody(api_type, host_config, uid_data, out);

  out.Puts("}\n");
}

ShaderCode GenerateVertexShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data)
{
  ShaderCode out;

  const bool per_pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool vertex_rounding = host_config.vertex_rounding;

  ShaderCode input_extract;

  out.Puts(s_lighting_struct);

  // uniforms
  out.Puts("UBO_BINDING(std140, 2) uniform VSBlock {\n");

  out.Puts(s_shader_uniforms);
  out.Puts("};\n");

  if (uid_data->vs_expand != VSExpand::None)
  {
    out.Puts("UBO_BINDING(std140, 4) uniform GSBlock {\n");
    out.Puts(s_geometry_shader_uniforms);
    out.Puts("};\n");

    if (api_type == APIType::D3D)
    {
      // D3D doesn't include the base vertex in SV_VertexID
      out.Puts("UBO_BINDING(std140, 5) uniform DX_Constants {\n"
                "  uint base_vertex;\n"
                "};\n"
                "\n");
    }
  }

  out.Puts("struct VS_OUTPUT {\n");
  GenerateVSOutputMembers(out, uid_data->numTexGens, host_config, "");
  out.Puts("};\n"
            "\n");

  WriteIsNanHeader(out, api_type);
  GenerateLightingShaderHeader(out, uid_data->lighting);

  if (uid_data->vs_expand == VSExpand::None)
  {
    out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float4 rawpos;\n", ShaderEnumValue(ShaderAttrib::Position), ShaderEnumName(ShaderAttrib::Position));
    if ((uid_data->components & VB_HAS_POSMTXIDX) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in uint4 posmtx;\n", ShaderEnumValue(ShaderAttrib::PositionMatrix), ShaderEnumName(ShaderAttrib::PositionMatrix));
    if ((uid_data->components & VB_HAS_NORMAL) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float3 rawnormal;\n", ShaderEnumValue(ShaderAttrib::Normal), ShaderEnumName(ShaderAttrib::Normal));
    if ((uid_data->components & VB_HAS_TANGENT) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float3 rawtangent;\n", ShaderEnumValue(ShaderAttrib::Tangent), ShaderEnumName(ShaderAttrib::Tangent));
    if ((uid_data->components & VB_HAS_BINORMAL) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float3 rawbinormal;\n", ShaderEnumValue(ShaderAttrib::Binormal), ShaderEnumName(ShaderAttrib::Binormal));

    if ((uid_data->components & VB_HAS_COL0) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float4 rawcolor0;\n", ShaderEnumValue(ShaderAttrib::Color0), ShaderEnumName(ShaderAttrib::Color0));
    if ((uid_data->components & VB_HAS_COL1) != 0)
      out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float4 rawcolor1;\n", ShaderEnumValue(ShaderAttrib::Color1), ShaderEnumName(ShaderAttrib::Color1));

    for (u32 i = 0; i < 8; ++i)
    {
      const u32 has_texmtx = (uid_data->components & (VB_HAS_TEXMTXIDX0 << i));

      if (uid_data->texGenInfo[i].texcoord_elem_count != 0 || has_texmtx != 0)
      {
        out.Printf("ATTRIBUTE_LOCATION(0x%xu /* %s */) in float%d rawtex%u;\n", ShaderEnumValue(ShaderAttrib::TexCoord0 + i), ShaderEnumName(ShaderAttrib::TexCoord0 + i), has_texmtx != 0 ? 3 : 2, i);
      }
    }
  }
  else
  {
    // Can't use float3, etc because we want 4-byte alignment
    out.Puts("uint4 unpack_ubyte4(uint value) {\n"
              "  return uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);\n"
              "}\n"
              "\n"
              "struct InputData {\n");
    if (uid_data->components & VB_HAS_POSMTXIDX)
    {
      out.Puts("  uint posmtx;\n");
      // Note: posmtx is handled in the matrix transform functions and
      // doesn't need to be added to 'input_extract'
    }
    if (uid_data->position_has_3_elems)
    {
      out.Puts("  float pos0;\n"
                "  float pos1;\n"
                "  float pos2;\n");
      input_extract.Puts("float4 rawpos = float4(i.pos0, i.pos1, i.pos2, 1.0f);\n");
    }
    else
    {
      out.Puts("  float pos0;\n"
                "  float pos1;\n");
      input_extract.Puts("float4 rawpos = float4(i.pos0, i.pos1, 0.0f, 1.0f);\n");
    }
    const char* names[3] = {"normal", "binormal", "tangent"};
    for (int i = 0; i < 3; i++)
    {
      if (uid_data->components & (VB_HAS_NORMAL << i))
      {
        out.Printf("  float %s0;\n"
                  "  float %s1;\n"
                  "  float %s2;\n", names[i], names[i], names[i]);
        input_extract.Printf("float3 raw%s = float3(i.%s0, i.%s1, i.%s2);\n", names[i], names[i], names[i], names[i]);
      }
    }
    for (int i = 0; i < 2; i++)
    {
      if (uid_data->components & (VB_HAS_COL0 << i))
      {
        out.Printf("  uint color%d;\n", i);
        input_extract.Printf("float4 rawcolor%d = float4(unpack_ubyte4(i.color%d)) / 255.0f;\n", i, i);
      }
    }
    for (int i = 0; i < 8; i++)
    {
      switch (uid_data->texGenInfo[i].texcoord_elem_count)
      {
      case 1:
        out.Printf("  float tex%d;\n", i);
        input_extract.Printf("float3 rawtex%d = float3(i.tex%d, 0.0f, 0.0f);\n", i, i);
        break;
      case 2:
        out.Printf("  float tex%d_0;\n"
                  "  float tex%d_1;\n", i, i);
        input_extract.Printf("float3 rawtex%d = float3(i.tex%d_0, i.tex%d_1, 0.0f);\n", i, i, i);
        break;
      case 3:
        out.Printf("  float tex%d_0;\n"
                  "  float tex%d_1;\n"
                  "  float tex%d_2;\n", i, i, i);
        input_extract.Printf("float3 rawtex%d = float3(i.tex%d_0, i.tex%d_1, i.tex%d_2);\n", i, i, i, i);
        break;
      }
    }
    out.Puts("};\n"
              "\n"
              "SSBO_BINDING(1) readonly restrict buffer InputBuffer {\n"
              "  InputData input_buffer[];\n"
              "};\n"
              "\n");
  }

  if (host_config.backend_geometry_shaders)
  {
    out.Puts("VARYING_LOCATION(0) out VertexData {\n");
    GenerateVSOutputMembers(out, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, false));
    out.Puts("} vs;\n");
  }
  else
  {
    // Let's set up attributes
    u32 counter = 0;
    out.Printf("VARYING_LOCATION(%u) %s out float4 colors_0;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    out.Printf("VARYING_LOCATION(%u) %s out float4 colors_1;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Printf("VARYING_LOCATION(%u) %s out float3 tex%u;\n", counter++, GetInterpolationQualifier(msaa, ssaa), i);
    }
    if (!host_config.fast_depth_calc)
    {
      out.Printf("VARYING_LOCATION(%u) %s out float4 clipPos;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    }
    if (per_pixel_lighting)
    {
      out.Printf("VARYING_LOCATION(%u) %s out float3 Normal;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
      out.Printf("VARYING_LOCATION(%u) %s out float3 WorldPos;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    }
  }

  // Note: this is done after to ensure above global variables are accessible
  WritePrimitiveExpand(api_type, host_config, uid_data, out);
  WriteTransformMatrices(api_type, host_config, uid_data, out);
  WriteTexCoordTransforms(api_type, host_config, uid_data, out);
  WriteVertexDefines(api_type, host_config, uid_data, out);
  WriteVertexStructs(api_type, host_config, uid_data, out);
  WriteEmulatedVertexBodyHeader(api_type, host_config, uid_data, out);

  out.Puts("void process_vertex(in DolphinVertexInput vertex_input, out DolphinVertexOutput vertex_output)\n");
  out.Puts("{\n");
  out.Puts("\tdolphin_process_emulated_vertex(vertex_input, vertex_output);\n");
  out.Puts("}\n");

  out.Puts("void main()\n"
            "{\n");

  if (uid_data->vs_expand != VSExpand::None)
  {
    out.Printf("InputData i = dolphin_primitive_expand_data(0);\n"
              "%s", input_extract.CStr());
  }

  out.Puts("VS_OUTPUT o;\n");

  // xfmem.numColorChans controls the number of color channels available to TEV, but we still need
  // to generate all channels here, as it can be used in texgen. Cel-damage is an example of this.
  out.Puts("float4 vertex_color_0, vertex_color_1;\n");

  // To use color 1, the vertex descriptor must have color 0 and 1.
  // If color 1 is present but not color 0, it is used for lighting channel 0.
  const bool use_color_1 =
      (uid_data->components & (VB_HAS_COL0 | VB_HAS_COL1)) == (VB_HAS_COL0 | VB_HAS_COL1);
  for (u32 color = 0; color < NUM_XF_COLOR_CHANNELS; color++)
  {
    if ((color == 0 || use_color_1) && (uid_data->components & (VB_HAS_COL0 << color)) != 0)
    {
      // Use color0 for channel 0, and color1 for channel 1 if both colors 0 and 1 are present.
      out.Printf("vertex_color_%u = rawcolor%u;\n", color, color);
    }
    else if (color == 0 && (uid_data->components & VB_HAS_COL1) != 0)
    {
      // Use color1 for channel 0 if color0 is not present.
      out.Printf("vertex_color_%u = rawcolor1;\n", color);
    }
    else
    {
      out.Printf("vertex_color_%u = missing_color_value;\n", color);
    }
  }

  out.Puts("\tDolphinVertexInput vertex_input;\n");
  out.Puts("\tvertex_input.color_0 = vertex_color_0;\n");
  out.Puts("\tvertex_input.color_1 = vertex_color_1;\n");
  out.Puts("\tvertex_input.position = rawpos;\n");

  if ((uid_data->components & VB_HAS_NORMAL) != 0)
  {
    out.Puts("\tvertex_input.normal = rawnormal;\n");
  }
  else
  {
    out.Puts("\tvertex_input.normal = "
              I_CACHED_NORMAL
              ".xyz;\n");
  }

  if ((uid_data->components & VB_HAS_BINORMAL) != 0)
  {
    out.Puts("\tvertex_input.binormal = rawbinormal;\n");
  }
  else
  {
    out.Puts("\tvertex_input.binormal = "
              I_CACHED_BINORMAL
              ".xyz;\n");
  }

  if ((uid_data->components & VB_HAS_TANGENT) != 0)
  {
    out.Puts("\tvertex_input.tangent = rawtangent;\n");
  }
  else
  {
    out.Puts("\tvertex_input.tangent = "
              I_CACHED_TANGENT
              ".xyz;\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];

    out.Puts("\t{\n");
    out.Puts("\t\tvec4 coord = vec4(0.0, 0.0, 1.0, 1.0);\n");
    switch (texinfo.sourcerow)
    {
    case SourceRow::Geom:
      out.Puts("\t\tcoord.xyz = rawpos.xyz;\n");
      break;
    case SourceRow::Normal:
      if ((uid_data->components & VB_HAS_NORMAL) != 0)
      {
        out.Puts("\t\tcoord.xyz = rawnormal.xyz;\n");
      }
      break;
    case SourceRow::Colors:
      ASSERT(!texinfo.is_regular_texgen);
      ASSERT(texinfo.other.texgentype == TexGenType::Color0 ||
             texinfo.other.texgentype == TexGenType::Color1);
      break;
    case SourceRow::BinormalT:
      if ((uid_data->components & VB_HAS_TANGENT) != 0)
      {
        out.Puts("\t\tcoord.xyz = rawtangent.xyz;\n");
      }
      break;
    case SourceRow::BinormalB:
      if ((uid_data->components & VB_HAS_BINORMAL) != 0)
      {
        out.Puts("\t\tcoord.xyz = rawbinormal.xyz;\n");
      }
      break;
    default:
      u32 texnum = static_cast<u32>(texinfo.sourcerow) - static_cast<u32>(SourceRow::Tex0);
      if (uid_data->texGenInfo[texnum].texcoord_elem_count != 0)
      {
        out.Printf("\t\tcoord = vec4(rawtex%u.x, rawtex%u.y, 1.0, 1.0);\n", texnum, texnum);
      }
      break;
    }
    // Input form of AB11 sets z element to 1.0

    if (texinfo.inputform == TexInputForm::AB11)
      out.Puts("\t\tcoord.z = 1.0;\n");

    // Convert NaNs to 1 - needed to fix eyelids in Shadow the Hedgehog during cutscenes
    // See https://bugs.dolphin-emu.org/issues/11458
    out.Puts("\t\t// Convert NaN to 1\n");
    out.Puts("\t\tif (dolphin_isnan(coord.x)) coord.x = 1.0;\n");
    out.Puts("\t\tif (dolphin_isnan(coord.y)) coord.y = 1.0;\n");
    out.Puts("\t\tif (dolphin_isnan(coord.z)) coord.z = 1.0;\n");

    out.Printf("\t\tvertex_input.texture_coord_%u = coord;\n", i);
    out.Puts("\t}\n");
  }

  // Initialize other texture coordinates that are unused
  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Printf("\tvertex_input.texture_coord_%u = vec4(0, 0, 0, 0);\n", i);
  }

  out.Puts("\tDolphinVertexOutput vertex_output;\n");
  out.Puts("\tprocess_vertex(vertex_input, vertex_output);\n");

  out.Puts("\to.pos = vec4(dot("
            I_PROJECTION
            "[0], vertex_output.position), dot("
            I_PROJECTION
            "[1], vertex_output.position), dot("
            I_PROJECTION
            "[2], vertex_output.position), dot("
            I_PROJECTION
            "[3], vertex_output.position));\n");
  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    out.Printf("\to.tex%u = vertex_output.texture_coord_%u;\n", i, i);
  }

  out.Puts("\to.colors_0 = vertex_output.color_0;\n");
  out.Puts("\to.colors_1 = vertex_output.color_1;\n");
  if (per_pixel_lighting)
  {
    out.Puts("\to.Normal = vertex_output.normal;\n");

    // TODO: Rename, this is actually in Viewspace...
    out.Puts("\to.WorldPos = vertex_output.position.xyz;\n");
  }

  if (uid_data->vs_expand == VSExpand::Line)
  {
    out.Puts("bool is_bottom = (gl_VertexID & 2) != 0;\n");
    out.Puts("// Line expansion\n"
              "int id_offset = 0;\n"
              "if (is_bottom) {\n"
              "  id_offset -= 1;\n"
              "} else {\n"
              "  id_offset += 1;\n"
              "}\n"
              "InputData other = dolphin_primitive_expand_data(id_offset);\n");
    if (uid_data->position_has_3_elems)
      out.Puts("float4 other_pos = float4(other.pos0, other.pos1, other.pos2, 1.0f);\n");
    else
      out.Puts("float4 other_pos = float4(other.pos0, other.pos1, 0.0f, 1.0f);\n");
    if (uid_data->components & VB_HAS_POSMTXIDX)
    {
      out.Puts("uint other_posidx = other.posmtx & 0xff;\n"
                "float4 other_p0 = "
                I_TRANSFORMMATRICES
                "[other_posidx];\n"
                "float4 other_p1 = "
                I_TRANSFORMMATRICES
                "[other_posidx + 1];\n"
                "float4 other_p2 = "
                I_TRANSFORMMATRICES
                "[other_posidx + 2];\n"
                "other_pos = float4(dot(other_p0, other_pos), dot(other_p1, other_pos), dot(other_p2, other_pos), 1.0f);\n");
    }
    else
    {
      out.Puts("other_pos = vec4(other_pos * dolphin_position_matrix(), 1.0);\n");
    }

    // Variable needed by GenerateVSLineExpansion
    out.Puts("bool is_right = (gl_VertexID & 1) != 0;\n");
    GenerateVSLineExpansion(out, "", uid_data->numTexGens);
  }
  else if (uid_data->vs_expand == VSExpand::Point)
  {
    // Variables needed by GenerateVSPointExpansion
    out.Puts("bool is_bottom = (gl_VertexID & 2) != 0;\n");
    out.Puts("bool is_right = (gl_VertexID & 1) != 0;\n");
    out.Puts("// Point expansion\n");
    GenerateVSPointExpansion(out, "", uid_data->numTexGens);
  }

  // clipPos/w needs to be done in pixel shader, not here
  if (!host_config.fast_depth_calc)
    out.Puts("o.clipPos = o.pos;\n");

  // If we can disable the incorrect depth clipping planes using depth clamping, then we can do
  // our own depth clipping and calculate the depth range before the perspective divide if
  // necessary.
  if (host_config.backend_depth_clamp)
  {
    // Since we're adjusting z for the depth range before the perspective divide, we have to do our
    // own clipping. We want to clip so that -w <= z <= 0, which matches the console -1..0 range.
    // We adjust our depth value for clipping purposes to match the perspective projection in the
    // software backend, which is a hack to fix Sonic Adventure and Unleashed games.
    out.Puts("float clipDepth = o.pos.z * (1.0 - 1e-7);\n"
              "float clipDist0 = clipDepth + o.pos.w;\n"
              "float clipDist1 = -clipDepth;\n");         // Far: z > 0

    if (host_config.backend_geometry_shaders)
    {
      out.Puts("o.clipDist0 = clipDist0;\n"
                "o.clipDist1 = clipDist1;\n");
    }
  }
  else
  {
    // Same depth adjustment for Sonic. Without depth clamping, it unfortunately
    // affects non-clipping uses of depth too.
    out.Puts("o.pos.z = o.pos.z * (1.0 - 1e-7);\n");
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
  out.Puts("o.pos.z = o.pos.w * "
            I_PIXELCENTERCORRECTION
            ".w - o.pos.z * "
            I_PIXELCENTERCORRECTION
            ".z;\n");

  if (!host_config.backend_clip_control)
  {
    // If the graphics API doesn't support a depth range of 0..1, then we need to map z to
    // the -1..1 range. Unfortunately we have to use a subtraction, which is a lossy floating-point
    // operation that can introduce a round-trip error.
    out.Puts("o.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  }

  // Correct for negative viewports by mirroring all vertices. We need to negate the height here,
  // since the viewport height is already negated by the render backend.
  out.Puts("o.pos.xy *= sign("
            I_PIXELCENTERCORRECTION
            ".xy * float2(1.0, -1.0));\n");

  // The console GPU places the pixel center at 7/12 in screen space unless
  // antialiasing is enabled, while D3D and OpenGL place it at 0.5. This results
  // in some primitives being placed one pixel too far to the bottom-right,
  // which in turn can be critical if it happens for clear quads.
  // Hence, we compensate for this pixel center difference so that primitives
  // get rasterized correctly.
  out.Puts("o.pos.xy = o.pos.xy - o.pos.w * "
            I_PIXELCENTERCORRECTION
            ".xy;\n");

  if (vertex_rounding)
  {
    // By now our position is in clip space
    // however, higher resolutions than the Wii outputs
    // cause an additional pixel offset
    // due to a higher pixel density
    // we need to correct this by converting our
    // clip-space position into the Wii's screen-space
    // acquire the right pixel and then convert it back
    out.Puts("if (o.pos.w == 1.0f)\n"
              "{\n"
              "\tfloat ss_pixel_x = ((o.pos.x + 1.0f) * ("
              I_VIEWPORT_SIZE
              ".x * 0.5f));\n"
              "\tfloat ss_pixel_y = ((o.pos.y + 1.0f) * ("
              I_VIEWPORT_SIZE
              ".y * 0.5f));\n"
              "\tss_pixel_x = round(ss_pixel_x);\n"
              "\tss_pixel_y = round(ss_pixel_y);\n"
              "\to.pos.x = ((ss_pixel_x / ("
              I_VIEWPORT_SIZE
              ".x * 0.5f)) - 1.0f);\n"
              "\to.pos.y = ((ss_pixel_y / ("
              I_VIEWPORT_SIZE
              ".y * 0.5f)) - 1.0f);\n"
              "}\n");
  }

  if (host_config.backend_geometry_shaders)
  {
    AssignVSOutputMembers(out, "vs", "o", uid_data->numTexGens, host_config);
  }
  else
  {
    // TODO: Pass interface blocks between shader stages even if geometry shaders
    // are not supported, however that will require at least OpenGL 3.2 support.
    for (u32 i = 0; i < uid_data->numTexGens; ++i)
      out.Printf("tex%u.xyz = o.tex%u;\n", i, i);
    if (!host_config.fast_depth_calc)
      out.Puts("clipPos = o.clipPos;\n");
    if (per_pixel_lighting)
    {
      out.Puts("Normal = o.Normal;\n"
                "WorldPos = o.WorldPos;\n");
    }
    out.Puts("colors_0 = o.colors_0;\n"
              "colors_1 = o.colors_1;\n");
  }

  if (host_config.backend_depth_clamp)
  {
    out.Puts("gl_ClipDistance[0] = clipDist0;\n"
              "gl_ClipDistance[1] = clipDist1;\n");
  }

  // Vulkan NDC space has Y pointing down (right-handed NDC space).
  if (api_type == APIType::Vulkan)
    out.Puts("gl_Position = float4(o.pos.x, -o.pos.y, o.pos.z, o.pos.w);\n");
  else
    out.Puts("gl_Position = o.pos;\n");
  out.Puts("}\n");

  return out;
}

void WriteVertexBody(APIType api_type, const ShaderHostConfig& host_config,
                     const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Puts("\tvertex_output.position = vec4(vertex_input.position * dolphin_position_matrix(), 1.0);\n");

  out.Puts("\tvertex_output.normal = normalize(vertex_input.normal * dolphin_normal_matrix());\n");

  for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
  {
    out.Printf("\tvec4 vertex_lighting_%u = dolphin_calculate_lighting_chn%u(vertex_input.color_%u, vertex_output.position.xyz, vertex_output.normal);\n", chan, chan, chan);
    out.Printf("\tvertex_output.color_%u = vertex_lighting_%u;\n", chan, chan);
  }

  if (host_config.per_pixel_lighting)
  {
    // When per-pixel lighting is enabled, the vertex colors are passed through
    // unmodified so we can evaluate the lighting in the pixel shader.
    out.Puts("\tvertex_output.color_0 = vertex_input.color_0;\n");
    out.Puts("\tvertex_output.color_1 = vertex_input.color_1;\n");
  }
  else
  {
    // The number of colors available to TEV is determined by numColorChans.
    // We have to provide the fields to match the interface, so set to zero if it's not enabled.
    if (uid_data->numColorChans == 0)
      out.Puts("\tvertex_output.color_0 = vec4(0.0, 0.0, 0.0, 0.0);\n");
    if (uid_data->numColorChans <= 1)
      out.Puts("\tvertex_output.color_1 = vec4(0.0, 0.0, 0.0, 0.0);\n");
  }

  for (u32 i = 0; i < uid_data->numTexGens; ++i)
  {
    auto& texinfo = uid_data->texGenInfo[i];

    auto texgentype = texinfo.is_regular_texgen ? TexGenType::Regular : texinfo.other.texgentype;

    switch (texgentype)
    {
    case TexGenType::EmbossMap:  // calculate tex coords into bump map

      out.Puts("\t{\n");
      // transform the light dir into tangent space
      out.Printf("\t\tvec3 ldir = normalize(%s[%d].pos.xyz - vertex_output.position.xyz);\n", (I_LIGHTS), (texinfo.other.emboss_lightshift));

      out.Puts("\t\tvec3 tangent = vertex_input.tangent * dolphin_normal_matrix();\n");
      out.Puts("\t\tvec3 binormal = vertex_input.binormal * dolphin_normal_matrix();\n");
      out.Printf("\t\tvertex_output.texture_coord_%u.xyz = vertex_output.texture_coord_%d.xyz + vec3(dot(ldir, tangent), dot(ldir, binormal), 0.0);\n", i, texinfo.other.emboss_sourceshift);
      out.Puts("\t}\n");
      break;
    case TexGenType::Color0:
      out.Printf("\tvertex_output.texture_coord_%u.xyz = vec3(vertex_lighting_0.x, vertex_lighting_0.y, 1);\n", i);
      break;
    case TexGenType::Color1:
      out.Printf("\tvertex_output.texture_coord_%u.xyz = vec3(vertex_lighting_1.x, vertex_lighting_1.y, 1);\n", i);
      break;
    case TexGenType::Regular:
      out.Printf("\tvertex_output.texture_coord_%u = dolphin_transform_texcoord%u(vertex_input.texture_coord_%u);\n", i, i, i);
      break;
    };
  }

  // Fill out output that is unused
  for (u32 i = uid_data->numTexGens; i < 8; i++)
  {
    out.Printf("\tvertex_output.texture_coord_%u = vec3(0, 0, 0);\n", i);
  }
}
