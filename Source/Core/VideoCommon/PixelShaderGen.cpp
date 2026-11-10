// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PixelShaderGen.h"

#include <cstdarg>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"  // for texture projection mode

// TODO: Get rid of these
enum : u32
{
  C_COLORMATRIX = 0,                //  0
  C_COLORS = 0,                     //  0
  C_KCOLORS = C_COLORS + 4,         //  4
  C_ALPHA = C_KCOLORS + 4,          //  8
  C_TEXDIMS = C_ALPHA + 1,          //  9
  C_ZBIAS = C_TEXDIMS + 8,          // 17
  C_INDTEXSCALE = C_ZBIAS + 2,      // 19
  C_INDTEXMTX = C_INDTEXSCALE + 2,  // 21
  C_FOGCOLOR = C_INDTEXMTX + 6,     // 27
  C_FOGI = C_FOGCOLOR + 1,          // 28
  C_FOGF = C_FOGI + 1,              // 29
  C_ZSLOPE = C_FOGF + 2,            // 31
  C_EFBSCALE = C_ZSLOPE + 1,        // 32
  C_PENVCONST_END = C_EFBSCALE + 1
};

constexpr Common::EnumMap<const char*, KonstSel::K3_A> tev_ksel_table_c{
    "255,255,255",        // 1   = 0x00
    "223,223,223",        // 7_8 = 0x01
    "191,191,191",        // 3_4 = 0x02
    "159,159,159",        // 5_8 = 0x03
    "128,128,128",        // 1_2 = 0x04
    "96,96,96",           // 3_8 = 0x05
    "64,64,64",           // 1_4 = 0x06
    "32,32,32",           // 1_8 = 0x07
    "0,0,0",              // INVALID = 0x08
    "0,0,0",              // INVALID = 0x09
    "0,0,0",              // INVALID = 0x0a
    "0,0,0",              // INVALID = 0x0b
    I_KCOLORS "[0].rgb",  // K0 = 0x0C
    I_KCOLORS "[1].rgb",  // K1 = 0x0D
    I_KCOLORS "[2].rgb",  // K2 = 0x0E
    I_KCOLORS "[3].rgb",  // K3 = 0x0F
    I_KCOLORS "[0].rrr",  // K0_R = 0x10
    I_KCOLORS "[1].rrr",  // K1_R = 0x11
    I_KCOLORS "[2].rrr",  // K2_R = 0x12
    I_KCOLORS "[3].rrr",  // K3_R = 0x13
    I_KCOLORS "[0].ggg",  // K0_G = 0x14
    I_KCOLORS "[1].ggg",  // K1_G = 0x15
    I_KCOLORS "[2].ggg",  // K2_G = 0x16
    I_KCOLORS "[3].ggg",  // K3_G = 0x17
    I_KCOLORS "[0].bbb",  // K0_B = 0x18
    I_KCOLORS "[1].bbb",  // K1_B = 0x19
    I_KCOLORS "[2].bbb",  // K2_B = 0x1A
    I_KCOLORS "[3].bbb",  // K3_B = 0x1B
    I_KCOLORS "[0].aaa",  // K0_A = 0x1C
    I_KCOLORS "[1].aaa",  // K1_A = 0x1D
    I_KCOLORS "[2].aaa",  // K2_A = 0x1E
    I_KCOLORS "[3].aaa",  // K3_A = 0x1F
};

constexpr Common::EnumMap<const char*, KonstSel::K3_A> tev_ksel_table_a{
    "255",              // 1   = 0x00
    "223",              // 7_8 = 0x01
    "191",              // 3_4 = 0x02
    "159",              // 5_8 = 0x03
    "128",              // 1_2 = 0x04
    "96",               // 3_8 = 0x05
    "64",               // 1_4 = 0x06
    "32",               // 1_8 = 0x07
    "0",                // INVALID = 0x08
    "0",                // INVALID = 0x09
    "0",                // INVALID = 0x0a
    "0",                // INVALID = 0x0b
    "0",                // INVALID = 0x0c
    "0",                // INVALID = 0x0d
    "0",                // INVALID = 0x0e
    "0",                // INVALID = 0x0f
    I_KCOLORS "[0].r",  // K0_R = 0x10
    I_KCOLORS "[1].r",  // K1_R = 0x11
    I_KCOLORS "[2].r",  // K2_R = 0x12
    I_KCOLORS "[3].r",  // K3_R = 0x13
    I_KCOLORS "[0].g",  // K0_G = 0x14
    I_KCOLORS "[1].g",  // K1_G = 0x15
    I_KCOLORS "[2].g",  // K2_G = 0x16
    I_KCOLORS "[3].g",  // K3_G = 0x17
    I_KCOLORS "[0].b",  // K0_B = 0x18
    I_KCOLORS "[1].b",  // K1_B = 0x19
    I_KCOLORS "[2].b",  // K2_B = 0x1A
    I_KCOLORS "[3].b",  // K3_B = 0x1B
    I_KCOLORS "[0].a",  // K0_A = 0x1C
    I_KCOLORS "[1].a",  // K1_A = 0x1D
    I_KCOLORS "[2].a",  // K2_A = 0x1E
    I_KCOLORS "[3].a",  // K3_A = 0x1F
};

constexpr Common::EnumMap<const char*, TevColorArg::Zero> tev_c_input_table{
    "prev.rgb",           // CPREV,
    "prev.aaa",           // APREV,
    "c0.rgb",             // C0,
    "c0.aaa",             // A0,
    "c1.rgb",             // C1,
    "c1.aaa",             // A1,
    "c2.rgb",             // C2,
    "c2.aaa",             // A2,
    "textemp.rgb",        // TEXC,
    "textemp.aaa",        // TEXA,
    "rastemp.rgb",        // RASC,
    "rastemp.aaa",        // RASA,
    "int3(255,255,255)",  // ONE
    "int3(128,128,128)",  // HALF
    "konsttemp.rgb",      // KONST
    "int3(0,0,0)",        // ZERO
};

constexpr Common::EnumMap<const char*, TevAlphaArg::Zero> tev_a_input_table{
    "prev.a",       // APREV,
    "c0.a",         // A0,
    "c1.a",         // A1,
    "c2.a",         // A2,
    "textemp.a",    // TEXA,
    "rastemp.a",    // RASA,
    "konsttemp.a",  // KONST,  (hw1 had quarter)
    "0",            // ZERO
};

constexpr Common::EnumMap<const char*, RasColorChan::Zero> tev_ras_table{
    "iround(col0 * 255.0)",
    "iround(col1 * 255.0)",
    "ERROR13",                                              // 2
    "ERROR14",                                              // 3
    "ERROR15",                                              // 4
    "(int4(1, 1, 1, 1) * alphabump)",                       // bump alpha (0..248)
    "(int4(1, 1, 1, 1) * (alphabump | (alphabump >> 5)))",  // normalized bump alpha (0..255)
    "int4(0, 0, 0, 0)",                                     // zero
};

constexpr Common::EnumMap<const char*, TevOutput::Color2> tev_c_output_table{
    "prev.rgb",
    "c0.rgb",
    "c1.rgb",
    "c2.rgb",
};

constexpr Common::EnumMap<const char*, TevOutput::Color2> tev_a_output_table{
    "prev.a",
    "c0.a",
    "c1.a",
    "c2.a",
};

constexpr Common::EnumMap<char, ColorChannel::Alpha> rgba_swizzle{'r', 'g', 'b', 'a'};

PixelShaderUid GetPixelShaderUid()
{
  PixelShaderUid out;

  pixel_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->useDstAlpha = bpmem.dstalpha.enable && bpmem.blendmode.alpha_update &&
                          bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;

  uid_data->genMode_numindstages = bpmem.genMode.numindstages;
  uid_data->genMode_numtevstages = bpmem.genMode.numtevstages;
  uid_data->genMode_numtexgens = bpmem.genMode.numtexgens;
  uid_data->bounding_box = g_ActiveConfig.bBBoxEnable && g_bounding_box->IsEnabled();
  uid_data->rgba6_format =
      bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24 && !g_ActiveConfig.bForceTrueColor;
  uid_data->dither = bpmem.blendmode.dither && uid_data->rgba6_format;
  uid_data->uint_output = bpmem.blendmode.UseLogicOp();

  u32 numStages = uid_data->genMode_numtevstages + 1;

  uid_data->Pretest = bpmem.alpha_test.TestResult();
  uid_data->ztest = bpmem.GetEmulatedZ();
  if (uid_data->ztest == EmulatedZ::Early &&
      (g_ActiveConfig.bFastDepthCalc ||
       bpmem.alpha_test.TestResult() == AlphaTestResult::Undetermined)
      // We can't allow early_ztest for zfreeze because depth is overridden per-pixel.
      // This means it's impossible for zcomploc to be emulated on a zfrozen polygon.
      && !bpmem.genMode.zfreeze)
  {
    uid_data->ztest = EmulatedZ::ForcedEarly;
  }

  const bool forced_early_z = uid_data->ztest == EmulatedZ::ForcedEarly;
  const bool per_pixel_depth =
      (bpmem.ztex2.op != ZTexOp::Disabled && uid_data->ztest == EmulatedZ::Late) ||
      (!g_ActiveConfig.bFastDepthCalc && bpmem.zmode.test_enable && !forced_early_z) ||
      (bpmem.zmode.test_enable && bpmem.genMode.zfreeze);

  uid_data->per_pixel_depth = per_pixel_depth;

  if (g_ActiveConfig.bEnablePixelLighting)
  {
    uid_data->numColorChans = xfmem.numChan.numColorChans;
    GetLightingShaderUid(uid_data->lighting);
  }

  if (uid_data->genMode_numtexgens > 0)
  {
    for (unsigned int i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      // optional perspective divides
      uid_data->texMtxInfo_n_projection |= static_cast<u32>(xfmem.texMtxInfo[i].projection.Value())
                                           << i;
    }
  }

  // indirect texture map lookup
  int nIndirectStagesUsed = 0;
  for (unsigned int i = 0; i < numStages; ++i)
  {
    if (bpmem.tevind[i].IsActive())
      nIndirectStagesUsed |= 1 << bpmem.tevind[i].bt;
  }

  uid_data->nIndirectStagesUsed = nIndirectStagesUsed;
  for (u32 i = 0; i < uid_data->genMode_numindstages; ++i)
  {
    if (uid_data->nIndirectStagesUsed & (1 << i))
      uid_data->SetTevindrefValues(i, bpmem.tevindref.getTexCoord(i), bpmem.tevindref.getTexMap(i));
  }

  for (unsigned int n = 0; n < numStages; n++)
  {
    uid_data->stagehash[n].tevorders_texcoord = bpmem.tevorders[n / 2].getTexCoord(n & 1);
    uid_data->stagehash[n].tevind = bpmem.tevind[n].hex;

    TevStageCombiner::ColorCombiner& cc = bpmem.combiners[n].colorC;
    TevStageCombiner::AlphaCombiner& ac = bpmem.combiners[n].alphaC;
    uid_data->stagehash[n].cc = cc.hex & 0xFFFFFF;
    uid_data->stagehash[n].ac = ac.hex & 0xFFFFF0;  // Storing rswap and tswap later

    if (cc.a == TevColorArg::RasAlpha || cc.a == TevColorArg::RasColor ||
        cc.b == TevColorArg::RasAlpha || cc.b == TevColorArg::RasColor ||
        cc.c == TevColorArg::RasAlpha || cc.c == TevColorArg::RasColor ||
        cc.d == TevColorArg::RasAlpha || cc.d == TevColorArg::RasColor ||
        ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
        ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha)
    {
      const auto ras_swap_table = bpmem.tevksel.GetSwapTable(bpmem.combiners[n].alphaC.rswap);
      uid_data->stagehash[n].ras_swap_r = ras_swap_table[ColorChannel::Red];
      uid_data->stagehash[n].ras_swap_g = ras_swap_table[ColorChannel::Green];
      uid_data->stagehash[n].ras_swap_b = ras_swap_table[ColorChannel::Blue];
      uid_data->stagehash[n].ras_swap_a = ras_swap_table[ColorChannel::Alpha];
      uid_data->stagehash[n].tevorders_colorchan = bpmem.tevorders[n / 2].getColorChan(n & 1);
    }

    uid_data->stagehash[n].tevorders_enable = bpmem.tevorders[n / 2].getEnable(n & 1);
    if (uid_data->stagehash[n].tevorders_enable)
    {
      const auto tex_swap_table = bpmem.tevksel.GetSwapTable(bpmem.combiners[n].alphaC.tswap);
      uid_data->stagehash[n].tex_swap_r = tex_swap_table[ColorChannel::Red];
      uid_data->stagehash[n].tex_swap_g = tex_swap_table[ColorChannel::Green];
      uid_data->stagehash[n].tex_swap_b = tex_swap_table[ColorChannel::Blue];
      uid_data->stagehash[n].tex_swap_a = tex_swap_table[ColorChannel::Alpha];
      uid_data->stagehash[n].tevorders_texmap = bpmem.tevorders[n / 2].getTexMap(n & 1);
    }

    if (cc.a == TevColorArg::Konst || cc.b == TevColorArg::Konst || cc.c == TevColorArg::Konst ||
        cc.d == TevColorArg::Konst || ac.a == TevAlphaArg::Konst || ac.b == TevAlphaArg::Konst ||
        ac.c == TevAlphaArg::Konst || ac.d == TevAlphaArg::Konst)
    {
      uid_data->stagehash[n].tevksel_kc = bpmem.tevksel.GetKonstColor(n);
      uid_data->stagehash[n].tevksel_ka = bpmem.tevksel.GetKonstAlpha(n);
    }
  }

#define MY_STRUCT_OFFSET(str, elem) ((u32)((u64) & (str).elem - (u64) & (str)))
  uid_data->num_values = (g_ActiveConfig.bEnablePixelLighting) ?
                             sizeof(*uid_data) :
                             MY_STRUCT_OFFSET(*uid_data, stagehash[numStages]);

  // NOTE: Fragment may not be discarded if alpha test always fails and early depth test is enabled
  // (in this case we need to write a depth value if depth test passes regardless of the alpha
  // testing result)
  if (uid_data->Pretest == AlphaTestResult::Undetermined ||
      (uid_data->Pretest == AlphaTestResult::Fail && uid_data->ztest == EmulatedZ::Late))
  {
    uid_data->alpha_test_comp0 = bpmem.alpha_test.comp0;
    uid_data->alpha_test_comp1 = bpmem.alpha_test.comp1;
    uid_data->alpha_test_logic = bpmem.alpha_test.logic;
  }

  uid_data->zfreeze = bpmem.genMode.zfreeze;
  uid_data->ztex_op = bpmem.ztex2.op;

  uid_data->fog_fsel = bpmem.fog.c_proj_fsel.fsel;
  uid_data->fog_proj = bpmem.fog.c_proj_fsel.proj;
  uid_data->fog_RangeBaseEnabled = bpmem.fogRange.Base.Enabled;

  return out;
}

void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid)
{
  pixel_shader_uid_data* const uid_data = uid->GetUidData();

  // OpenGL and Vulkan convert implicitly normalized color outputs to their uint representation.
  // Therefore, it is not necessary to use a uint output on these backends. We also disable the
  // uint output when logic op is not supported (i.e. driver/device does not support D3D11.1).
  if (api_type != APIType::D3D || !host_config.backend_logic_op)
    uid_data->uint_output = 0;

  // If bounding box is enabled when a UID cache is created, then later disabled, we shouldn't
  // emit the bounding box portion of the shader.
  uid_data->bounding_box &= host_config.bounding_box && host_config.backend_bbox;
}

void WritePixelShaderCommonHeader(ShaderCode& out, APIType api_type,
                                  const ShaderHostConfig& host_config, bool bounding_box)
{
  // dot product for integer vectors
  out.Puts("int idot(int3 x, int3 y)\n"
            "{\n"
            "\tint3 tmp = x * y;\n"
            "\treturn tmp.x + tmp.y + tmp.z;\n"
            "}\n");

  out.Puts("int idot(int4 x, int4 y)\n"
            "{\n"
            "\tint4 tmp = x * y;\n"
            "\treturn tmp.x + tmp.y + tmp.z + tmp.w;\n"
            "}\n"
            "\n");

  // rounding + casting to integer at once in a single function
  out.Puts("int  iround(float  x) { return int (round(x)); }\n"
            "int2 iround(float2 x) { return int2(round(x)); }\n"
            "int3 iround(float3 x) { return int3(round(x)); }\n"
            "int4 iround(float4 x) { return int4(round(x)); }\n"
            "\n");

  out.Puts("SAMPLER_BINDING(0) uniform sampler2DArray samp[8];\n");
  out.Puts("\n");

  out.Puts("UBO_BINDING(std140, 1) uniform PSBlock {\n");

  out.Puts("\tint4 "
            I_COLORS
            "[4];\n"
            "\tint4 "
            I_KCOLORS
            "[4];\n"
            "\tint4 "
            I_ALPHA
            ";\n"
            "\tint4 "
            I_TEXDIMS
            "[8];\n"
            "\tint4 "
            I_ZBIAS
            "[2];\n"
            "\tint4 "
            I_INDTEXSCALE
            "[2];\n"
            "\tint4 "
            I_INDTEXMTX
            "[6];\n"
            "\tint4 "
            I_FOGCOLOR
            ";\n"
            "\tint4 "
            I_FOGI
            ";\n"
            "\tfloat4 "
            I_FOGF
            ";\n"
            "\tfloat4 "
            I_FOGRANGE
            "[3];\n"
            "\tfloat4 "
            I_ZSLOPE
            ";\n"
            "\tfloat2 "
            I_EFBSCALE
            ";\n"
            "\tuint  bpmem_genmode;\n"
            "\tuint  bpmem_alphaTest;\n"
            "\tuint  bpmem_fogParam3;\n"
            "\tuint  bpmem_fogRangeBase;\n"
            "\tuint  bpmem_dstalpha;\n"
            "\tuint  bpmem_ztex_op;\n"
            "\tbool  bpmem_late_ztest;\n"
            "\tbool  bpmem_rgba6_format;\n"
            "\tbool  bpmem_dither;\n"
            "\tbool  bpmem_bounding_box;\n"
            "\tuint4 bpmem_pack1[16];\n"
            "\tuint4 bpmem_pack2[8];\n"
            "\tint4  konstLookup[32];\n"
            "\tbool  blend_enable;\n"
            "\tuint  blend_src_factor;\n"
            "\tuint  blend_src_factor_alpha;\n"
            "\tuint  blend_dst_factor;\n"
            "\tuint  blend_dst_factor_alpha;\n"
            "\tbool  blend_subtract;\n"
            "\tbool  blend_subtract_alpha;\n"
            "\tbool  logic_op_enable;\n"
            "\tuint  logic_op_mode;\n"
            "\tuint  time_ms;\n"
            "\tuint  xfmem_numTexGen;\n"
            "};\n"
            "\n");
  out.Puts("#define bpmem_combiners(i) (bpmem_pack1[(i)].xy)\n"
            "#define bpmem_tevind(i) (bpmem_pack1[(i)].z)\n"
            "#define bpmem_iref(i) (bpmem_pack1[(i)].w)\n"
            "#define bpmem_tevorder(i) (bpmem_pack2[(i)].x)\n"
            "#define bpmem_tevksel(i) (bpmem_pack2[(i)].y)\n"
            "#define samp_texmode0(i) (bpmem_pack2[(i)].z)\n"
            "#define samp_texmode1(i) (bpmem_pack2[(i)].w)\n"
            "\n");

  if (host_config.per_pixel_lighting)
  {
    out.Puts(s_lighting_struct);

    out.Puts("UBO_BINDING(std140, 2) uniform VSBlock {\n");

    out.Puts(s_shader_uniforms);
    out.Puts("};\n");
  }

  if (bounding_box)
  {
    out.Puts("SSBO_BINDING(0) coherent buffer BBox {\n"
              "  int bbox_data[4];\n"
              "};");

    out.Printf("\n"
              "#define bbox_left bbox_data[0]\n"
              "#define bbox_right bbox_data[1]\n"
              "#define bbox_top bbox_data[2]\n"
              "#define bbox_bottom bbox_data[3]\n"
              "\n"
              "void UpdateBoundingBoxBuffer(int2 min_pos, int2 max_pos) {\n"
              "  if (bbox_left > min_pos.x)\n"
              "    atomicMin(bbox_left, min_pos.x);\n"
              "  if (bbox_right < max_pos.x)\n"
              "    atomicMax(bbox_right, max_pos.x);\n"
              "  if (bbox_top > min_pos.y)\n"
              "    atomicMin(bbox_top, min_pos.y);\n"
              "  if (bbox_bottom < max_pos.y)\n"
              "    atomicMax(bbox_bottom, max_pos.y);\n"
              "}\n"
              "\n"
              "void UpdateBoundingBox(float2 rawpos) {\n"
              "  // We only want to include coordinates for pixels aligned with the native resolution pixel centers.\n"
              "  // This makes bounding box sizes more accurate (though not perfect) at higher resolutions,\n"
              "  // avoiding EFB copy buffer overflow in affected games.\n"
              "  //\n"
              "  // For a more detailed explanation, see https://dolp.in/pr9801\n"
              "  int2 int_efb_scale = iround(1.0 / %s.xy);\n"
              "  if (int(rawpos.x) %% int_efb_scale.x != int_efb_scale.x >> 1 ||\n"
              "      int(rawpos.y) %% int_efb_scale.y != int_efb_scale.y >> 1)  // right shift for fast divide by two\n"
              "  {\n"
              "    return;\n"
              "  }\n"
              "\n"
              "  // The rightmost shaded pixel is not included in the right bounding box register,\n"
              "  // such that width = right - left + 1. This has been verified on hardware.\n"
              "  int2 pos = int2(rawpos * %s.xy);\n"
              "\n"
              "#ifdef API_OPENGL\n"
              "  // We need to invert the Y coordinate due to OpenGL's lower-left origin\n"
              "  pos.y = %u - pos.y - 1;\n"
              "#endif\n"
              "\n"
              "  // The GC/Wii GPU rasterizes in 2x2 pixel groups, so bounding box values will be rounded to the\n"
              "  // extents of these groups, rather than the exact pixel.\n"
              "  int2 pos_tl = pos & ~1;  // round down to even\n"
              "  int2 pos_br = pos | 1;   // round up to odd\n"
              "\n"
              "#if defined(SUPPORTS_SUBGROUP_REDUCTION) && !defined(BROKEN_SUBGROUP_WITH_DISCARD)\n"
              "  if (!IS_HELPER_INVOCATION)\n"
              "  {\n"
              "    SUBGROUP_MIN(pos_tl);\n"
              "    SUBGROUP_MAX(pos_br);\n"
              "    if (IS_FIRST_ACTIVE_INVOCATION)\n"
              "      UpdateBoundingBoxBuffer(pos_tl, pos_br);\n"
              "  }\n"
              "#else\n"
              "  UpdateBoundingBoxBuffer(pos_tl, pos_br);\n"
              "#endif\n"
              "}\n"
              "\n", I_EFBSCALE, I_EFBSCALE, EFB_HEIGHT);
  }

  if (host_config.manual_texture_sampling)
  {
    out.Puts("\n"
              "int4 readTexture(in sampler2DArray tex, uint u, uint v, int layer, int lod) {\n"
              "  return iround(texelFetch(tex, int3(u, v, layer), lod) * 255.0);\n"
              "}\n"
              "\n"
              "int4 readTextureLinear(in sampler2DArray tex, uint2 uv1, uint2 uv2, int layer, int lod, int2 frac_uv) {");

    out.Puts("\n"
              "  int4 result =\n"
              "    readTexture(tex, uv1.x, uv1.y, layer, lod) * (128 - frac_uv.x) * (128 - frac_uv.y) +\n"
              "    readTexture(tex, uv2.x, uv1.y, layer, lod) * (      frac_uv.x) * (128 - frac_uv.y) +\n"
              "    readTexture(tex, uv1.x, uv2.y, layer, lod) * (128 - frac_uv.x) * (      frac_uv.y) +\n"
              "    readTexture(tex, uv2.x, uv2.y, layer, lod) * (      frac_uv.x) * (      frac_uv.y);\n"
              "  return result >> 14;\n"
              "}\n");

    if (host_config.manual_texture_sampling_custom_texture_sizes)
    {
      // This is slower, and doesn't result in the same odd behavior that happens on console when
      // wrapping with non-power-of-2 sizes, but it's fine for custom textures to have non-console
      // behavior.
      out.Printf("\n"
                "// Both GLSL and HLSL produce undefined values when the modulo operator (%%) is used with a negative\n"
                "// dividend and a positive divisor.  We want a positive value such that SafeModulo(-1, 3) is 2.\n"
                "int SafeModulo(int dividend, int divisor) {\n"
                "  if (dividend >= 0) {\n"
                "    return dividend %% divisor;\n"
                "  } else {\n"
                "    // This works because ~x is the same as -x - 1.\n"
                "    // `~x %% 5` over -5 to -1 gives 4, 3, 2, 1, 0.  `4 - (~x %% 5)` gives 0, 1, 2, 3, 4.\n"
                "    return (divisor - 1) - (~dividend %% divisor);\n"
                "  }\n"
                "}\n"
                "\n"
                "uint WrapCoord(int coord, uint wrap, int size) {\n"
                "  switch (wrap) {\n"
                "    case 0x%xu /* %s */:\n"
                "    default: // confirmed that clamp is used for invalid (3) via hardware test\n"
                "      return uint(clamp(coord, 0, size - 1));\n"
                "    case 0x%xu /* %s */:\n"
                "      return uint(SafeModulo(coord, size));  // coord %% size\n"
                "    case 0x%xu /* %s */:\n"
                "      if (SafeModulo(coord, 2 * size) >= size) {  // coord %% (2 * size)\n"
                "        coord = ~coord;\n"
                "      }\n"
                "      return uint(SafeModulo(coord, size));  // coord %% size\n"
                "  }\n"
                "}\n", ShaderEnumValue(WrapMode::Clamp), ShaderEnumName(WrapMode::Clamp), ShaderEnumValue(WrapMode::Repeat), ShaderEnumName(WrapMode::Repeat), ShaderEnumValue(WrapMode::Mirror), ShaderEnumName(WrapMode::Mirror));
    }
    else
    {
      out.Printf("\n"
                "uint WrapCoord(int coord, uint wrap, int size) {\n"
                "  switch (wrap) {\n"
                "    case 0x%xu /* %s */:\n"
                "    default: // confirmed that clamp is used for invalid (3) via hardware test\n"
                "      return uint(clamp(coord, 0, size - 1));\n"
                "    case 0x%xu /* %s */:\n"
                "      return uint(coord & (size - 1));\n"
                "    case 0x%xu /* %s */:\n"
                "      if ((coord & size) != 0) {\n"
                "        coord = ~coord;\n"
                "      }\n"
                "      return uint(coord & (size - 1));\n"
                "  }\n"
                "}\n", ShaderEnumValue(WrapMode::Clamp), ShaderEnumName(WrapMode::Clamp), ShaderEnumValue(WrapMode::Repeat), ShaderEnumName(WrapMode::Repeat), ShaderEnumValue(WrapMode::Mirror), ShaderEnumName(WrapMode::Mirror));
    }
  }

  out.Puts("\n"
            "int4 sampleTexture(uint texmap, in sampler2DArray tex, int2 uv, int layer) {\n");

  if (!host_config.manual_texture_sampling)
  {
    out.Puts("  float size_s = float("
              I_TEXDIMS
              "[texmap].x * 128);\n"
              "  float size_t = float("
              I_TEXDIMS
              "[texmap].y * 128);\n"
              "  float3 coords = float3(float(uv.x) / size_s, float(uv.y) / size_t, layer);\n");
    if (!host_config.backend_sampler_lod_bias)
    {
      out.Printf("  uint texmode0 = samp_texmode0(texmap);\n"
                "  float lod_bias = float(%s) / 256.0f;\n"
                "  return iround(255.0 * texture(tex, coords, lod_bias));\n", BitfieldExtract<&SamplerState::TM0::lod_bias>("texmode0").text);
    }
    else
    {
      out.Puts("  return iround(255.0 * texture(tex, coords));\n");
    }

    out.Puts("}\n");
  }
  else
  {
    out.Printf("\n"
              "  uint texmode0 = samp_texmode0(texmap);\n"
              "  uint texmode1 = samp_texmode1(texmap);\n"
              "\n"
              "  uint wrap_s = %s;\n"
              "  uint wrap_t = %s;\n"
              "  bool mag_linear = %s != 0u;\n"
              "  bool mipmap_linear = %s != 0u;\n"
              "  bool min_linear = %s != 0u;\n"
              "  bool diag_lod = %s != 0u;\n"
              "  int lod_bias = %s;\n"
              "  // uint max_aniso = TODO;\n"
              "  bool lod_clamp = %s != 0u;\n"
              "  int min_lod = int(%s);\n"
              "  int max_lod = int(%s);\n", BitfieldExtract<&SamplerState::TM0::wrap_u>("texmode0").text, BitfieldExtract<&SamplerState::TM0::wrap_v>("texmode0").text, BitfieldExtract<&SamplerState::TM0::mag_filter>("texmode0").text, BitfieldExtract<&SamplerState::TM0::mipmap_filter>("texmode0").text, BitfieldExtract<&SamplerState::TM0::min_filter>("texmode0").text, BitfieldExtract<&SamplerState::TM0::diag_lod>("texmode0").text, BitfieldExtract<&SamplerState::TM0::lod_bias>("texmode0").text, BitfieldExtract<&SamplerState::TM0::lod_clamp>("texmode0").text, BitfieldExtract<&SamplerState::TM1::min_lod>("texmode1").text, BitfieldExtract<&SamplerState::TM1::max_lod>("texmode1").text);

    if (host_config.manual_texture_sampling_custom_texture_sizes)
    {
      out.Puts("\n"
                "  int native_size_s = "
                I_TEXDIMS
                "[texmap].x;\n"
                "  int native_size_t = "
                I_TEXDIMS
                "[texmap].y;\n");

      out.Puts("\n"
                "  int3 size = textureSize(tex, 0);\n"
                "  int size_s = size.x;\n"
                "  int size_t = size.y;\n"
                "  int num_layers = size.z;\n");
      if (g_backend_info.bSupportsTextureQueryLevels)
      {
        out.Puts("  int number_of_levels = textureQueryLevels(tex);\n");
      }
      else
      {
        out.Puts("  int number_of_levels = 256;  // textureQueryLevels is not supported\n");
        ERROR_LOG_FMT(VIDEO, "textureQueryLevels is not supported!  Odd graphical results may "
                             "occur if custom textures are in use!");
      }

      out.Puts("\n"
                "  // Prevent out-of-bounds LOD values when using custom textures\n"
                "  max_lod = min(max_lod, (number_of_levels - 1) << 4);\n"
                "  // Rescale uv to account for the new texture size\n"
                "  uv.x = (uv.x * size_s) / native_size_s;\n"
                "  uv.y = (uv.y * size_t) / native_size_t;\n"
                "  // Clamp layer as well (texture() automatically clamps, but texelFetch() doesn't)\n"
                "  layer = clamp(layer, 0, num_layers - 1);\n");
    }
    else
    {
      out.Puts("\n"
                "  int size_s = "
                I_TEXDIMS
                "[texmap].x;\n"
                "  int size_t = "
                I_TEXDIMS
                "[texmap].y;\n");
    }

    if (g_backend_info.bSupportsCoarseDerivatives)
    {
      // The software renderer uses the equivalent of coarse derivatives, so use them here for
      // consistency.  This hasn't been hardware tested.
      // Note that bSupportsCoarseDerivatives being false only means dFdxCoarse and dFdxFine don't
      // exist.  The GPU may still implement dFdx using coarse derivatives; we just don't have the
      // ability to specifically require it.
      out.Puts("\n"
                "  float2 uv_delta_x = abs(dFdxCoarse(float2(uv)));\n"
                "  float2 uv_delta_y = abs(dFdyCoarse(float2(uv)));\n");
    }
    else
    {
      out.Puts("\n"
                "  float2 uv_delta_x = abs(dFdx(float2(uv)));\n"
                "  float2 uv_delta_y = abs(dFdy(float2(uv)));\n");
    }

    // TODO: LOD bias is normally S2.5 (Dolphin uses S7.8 for arbitrary mipmap detection and higher
    // IRs), but (at least per the software renderer) actual LOD is S28.4.  How does this work?
    // Also, note that we can make some assumptions due to use of a SamplerState version of the BP
    // configuration, which tidies things compared to whatever nonsense games can put in.
    out.Puts("\n"
              "  float2 uv_delta = diag_lod ? uv_delta_x + uv_delta_y : max(uv_delta_x, uv_delta_y);\n"
              "  float max_delta = max(uv_delta.x / 128.0, uv_delta.y / 128.0);\n"
              "  // log2(x) is undefined if x <= 0, but in practice it seems log2(0) is -infinity, which becomes INT_MIN.\n"
              "  // If lod_bias is negative, adding it to INT_MIN causes an underflow, resulting in a large positive value.\n"
              "  // Hardware testing indicates that min_lod should be used when the derivative is 0.\n"
              "  int lod = max_delta == 0.0 ? min_lod : int(floor(log2(max_delta) * 16.0)) + (lod_bias >> 4);\n"
              "\n"
              "  bool is_linear = (lod > 0) ? min_linear : mag_linear;\n"
              "  lod = clamp(lod, min_lod, max_lod);\n"
              "  int base_lod = lod >> 4;\n"
              "  int frac_lod = lod & 15;\n"
              "  if (!mipmap_linear && frac_lod >= 8) {\n"
              "    // Round to nearest LOD in point mode\n"
              "    base_lod++;\n"
              "  }\n"
              "\n"
              "  if (is_linear) {\n"
              "    uint2 texuv1 = uint2(\n"
              "        WrapCoord(((uv.x >> base_lod) - 64) >> 7, wrap_s, size_s >> base_lod),\n"
              "        WrapCoord(((uv.y >> base_lod) - 64) >> 7, wrap_t, size_t >> base_lod));\n"
              "    uint2 texuv2 = uint2(\n"
              "        WrapCoord(((uv.x >> base_lod) + 64) >> 7, wrap_s, size_s >> base_lod),\n"
              "        WrapCoord(((uv.y >> base_lod) + 64) >> 7, wrap_t, size_t >> base_lod));\n"
              "    int2 frac_uv = int2(((uv.x >> base_lod) - 64) & 0x7f, ((uv.y >> base_lod) - 64) & 0x7f);\n"
              "\n"
              "    int4 result = readTextureLinear(tex, texuv1, texuv2, layer, base_lod, frac_uv);\n"
              "\n"
              "    if (frac_lod != 0 && mipmap_linear) {\n"
              "      texuv1 = uint2(\n"
              "          WrapCoord(((uv.x >> (base_lod + 1)) - 64) >> 7, wrap_s, size_s >> (base_lod + 1)),\n"
              "          WrapCoord(((uv.y >> (base_lod + 1)) - 64) >> 7, wrap_t, size_t >> (base_lod + 1)));\n"
              "      texuv2 = uint2(\n"
              "          WrapCoord(((uv.x >> (base_lod + 1)) + 64) >> 7, wrap_s, size_s >> (base_lod + 1)),\n"
              "          WrapCoord(((uv.y >> (base_lod + 1)) + 64) >> 7, wrap_t, size_t >> (base_lod + 1)));\n"
              "      frac_uv = int2(((uv.x >> (base_lod + 1)) - 64) & 0x7f, ((uv.y >> (base_lod + 1)) - 64) & 0x7f);\n"
              "\n"
              "      result *= 16 - frac_lod;\n"
              "      result += readTextureLinear(tex, texuv1, texuv2, layer, base_lod + 1, frac_uv) * frac_lod;\n"
              "      result >>= 4;\n"
              "    }\n"
              "\n"
              "    return result;\n"
              "  } else {\n"
              "    uint2 texuv = uint2(\n"
              "        WrapCoord(uv.x >> (7 + base_lod), wrap_s, size_s >> base_lod),\n"
              "        WrapCoord(uv.y >> (7 + base_lod), wrap_t, size_t >> base_lod));\n"
              "\n"
              "    int4 result = readTexture(tex, texuv.x, texuv.y, layer, base_lod);\n"
              "\n"
              "    if (frac_lod != 0 && mipmap_linear) {\n"
              "      texuv = uint2(\n"
              "          WrapCoord(uv.x >> (7 + base_lod + 1), wrap_s, size_s >> (base_lod + 1)),\n"
              "          WrapCoord(uv.y >> (7 + base_lod + 1), wrap_t, size_t >> (base_lod + 1)));\n"
              "\n"
              "      result *= 16 - frac_lod;\n"
              "      result += readTexture(tex, texuv.x, texuv.y, layer, base_lod + 1) * frac_lod;\n"
              "      result >>= 4;\n"
              "    }\n"
              "    return result;\n"
              "  }\n"
              "}\n");
  }
}

static void WriteStage(ShaderCode& out, const pixel_shader_uid_data* uid_data, int n,
                       APIType api_type, bool stereo);
static void WriteTevRegular(ShaderCode& out, const char* components, TevBias bias, TevOp op,
                            bool clamp, TevScale scale);
static void WriteAlphaTest(ShaderCode& out, const pixel_shader_uid_data* uid_data, APIType api_type,
                           bool per_pixel_depth, bool use_dual_source);
static void WriteFog(ShaderCode& out, const pixel_shader_uid_data* uid_data);
static void WriteLogicOp(ShaderCode& out, const pixel_shader_uid_data* uid_data);
static void WriteLogicOpBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data);
static void WriteColor(ShaderCode& out, APIType api_type, const pixel_shader_uid_data* uid_data,
                       bool use_dual_source);
static void WriteBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data);

static void WriteEmulatedFragmentBodyHeader(APIType api_type, const ShaderHostConfig& host_config,
                                            const pixel_shader_uid_data* uid_data, ShaderCode& out);
static void WriteFragmentDefinitions(APIType api_type, const ShaderHostConfig& host_config,
                                     const pixel_shader_uid_data* uid_data, ShaderCode& out);

ShaderCode GeneratePixelShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                   const pixel_shader_uid_data* uid_data)
{
  ShaderCode out;

  const bool per_pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  const u32 numStages = uid_data->genMode_numtevstages + 1;

  out.Puts("// Pixel Shader for TEV stages\n");
  out.Printf("// %u TEV stages, %d texgens, %d IND stages\n", numStages, uid_data->genMode_numtexgens, uid_data->genMode_numindstages);

  // Stuff that is shared between ubershaders and pixelgen.
  WriteBitfieldExtractHeader(out, api_type, host_config);

  WritePixelShaderCommonHeader(out, api_type, host_config, uid_data->bounding_box);

  out.Puts("\n"
            "#define sampleTextureWrapper(texmap, uv, layer) sampleTexture(texmap, samp[texmap], uv, layer)\n");

  if (uid_data->ztest == EmulatedZ::ForcedEarly)
  {
    // Zcomploc (aka early_ztest) is a way to control whether depth test is done before
    // or after texturing and alpha test. PC graphics APIs used to provide no way to emulate
    // this feature properly until 2012: Depth tests were always done after alpha testing.
    // Most importantly, it was not possible to write to the depth buffer without also writing
    // a color value (unless color writing was disabled altogether).

    // OpenGL 4.2 actually provides two extensions which can force an early z test:
    //  * ARB_image_load_store has 'layout(early_fragment_tests)' which forces the driver to do z
    //  and stencil tests early.
    //  * ARB_conservative_depth has 'layout(depth_unchanged) which signals to the driver that it
    //  can make optimisations
    //    which assume the pixel shader won't update the depth buffer.

    // early_fragment_tests is the best option, as it requires the driver to do early-z and defines
    // early-z exactly as
    // we expect, with discard causing the shader to exit with only the depth buffer updated.

    // Conservative depth's 'depth_unchanged' only hints to the driver that an early-z optimisation
    // can be made and
    // doesn't define what will happen if we discard the fragment. But the way modern graphics
    // hardware is implemented
    // means it is not unreasonable to expect the same behaviour as early_fragment_tests.
    // We can also assume that if a driver has gone out of its way to support conservative depth and
    // not image_load_store
    // as required by OpenGL 4.2 that it will be doing the optimisation.
    // If the driver doesn't actually do an early z optimisation, ZCompLoc will be broken and depth
    // will only be written
    // if the alpha test passes.

    // We support Conservative as a fallback, because many drivers based on Mesa haven't implemented
    // all of the
    // ARB_image_load_store extension yet.

    // This is a #define which signals whatever early-z method the driver supports.
    out.Puts("FORCE_EARLY_Z; \n");
  }

  const bool use_framebuffer_fetch = uid_data->blend_enable || uid_data->logic_op_enable ||
                                     uid_data->ztest == EmulatedZ::EarlyWithFBFetch;

#ifdef __APPLE__
  // Framebuffer fetch is only supported by Metal, so ensure that we're running Vulkan (MoltenVK)
  // if we want to use it.
  if (api_type == APIType::Vulkan || api_type == APIType::Metal)
  {
    if (!uid_data->no_dual_src)
    {
      out.Printf("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out vec4 %s;\n"
                "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out vec4 ocol1;\n", use_framebuffer_fetch ? "real_ocol0" : "ocol0");
    }
    else
    {
      // Metal doesn't support a single unified variable for both input and output,
      // so when using framebuffer fetch, we declare the input separately below.
      out.Printf("FRAGMENT_OUTPUT_LOCATION(0) out vec4 %s;\n", use_framebuffer_fetch ? "real_ocol0" : "ocol0");
    }

    if (use_framebuffer_fetch)
    {
      // rslc prints a subpass input as Metal's framebuffer fetch.
      out.Puts("INPUT_ATTACHMENT_BINDING(0, 0, 0) uniform subpassInput in_ocol0;\n");
    }
  }
  else
#endif
  {
    if (use_framebuffer_fetch)
    {
      out.Puts("FRAGMENT_OUTPUT_LOCATION(0) FRAGMENT_INOUT vec4 real_ocol0;\n");
    }
    else
    {
      out.Printf("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out %s ocol0;\n", uid_data->uint_output ? "uvec4" : "vec4");
    }

    if (!uid_data->no_dual_src)
    {
      out.Printf("%s out %s ocol1;\n", "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1)", uid_data->uint_output ? "uvec4" : "vec4");
    }
  }

  if (uid_data->per_pixel_depth)
    out.Puts("#define depth gl_FragDepth\n");

  if (host_config.backend_geometry_shaders)
  {
    out.Puts("VARYING_LOCATION(0) in VertexData {\n");
    GenerateVSOutputMembers(out, uid_data->genMode_numtexgens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, true));

    out.Puts("};\n");
    if (stereo && !host_config.backend_gl_layer_in_fs)
      out.Puts("flat in int layer;");
  }
  else
  {
    // Let's set up attributes
    u32 counter = 0;
    out.Printf("VARYING_LOCATION(%u) %s in float4 colors_0;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    out.Printf("VARYING_LOCATION(%u) %s in float4 colors_1;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    for (u32 i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      out.Printf("VARYING_LOCATION(%u) %s in float3 tex%u;\n", counter++, GetInterpolationQualifier(msaa, ssaa), i);
    }
    if (!host_config.fast_depth_calc)
    {
      out.Printf("VARYING_LOCATION(%u) %s in float4 clipPos;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    }
    if (per_pixel_lighting)
    {
      out.Printf("VARYING_LOCATION(%u) %s in float3 Normal;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
      out.Printf("VARYING_LOCATION(%u) %s in float3 WorldPos;\n", counter++, GetInterpolationQualifier(msaa, ssaa));
    }
  }

  if (per_pixel_lighting)
  {
    GenerateLightingShaderHeader(out, uid_data->lighting);
  }

  WriteFragmentDefinitions(api_type, host_config, uid_data, out);
  WriteEmulatedFragmentBodyHeader(api_type, host_config, uid_data, out);

  out.Puts("void process_fragment(in DolphinFragmentInput frag_input, out DolphinFragmentOutput frag_output)\n");
  out.Puts("{\n");
  out.Puts("\tdolphin_process_emulated_fragment(frag_input, frag_output);\n");
  out.Puts("}\n");

  out.Puts("void main()\n"
            "{\n");
  out.Puts("\tfloat4 rawpos = gl_FragCoord;\n");

  if (use_framebuffer_fetch)
  {
    // Store off a copy of the initial framebuffer value.
    //
    // If FB_FETCH_VALUE isn't defined (i.e. no special keyword for fetching from the
    // framebuffer), we read from real_ocol0.
    out.Puts("#ifdef FB_FETCH_VALUE\n"
              "\tfloat4 initial_ocol0 = FB_FETCH_VALUE;\n"
              "#else\n"
              "\tfloat4 initial_ocol0 = real_ocol0;\n"
              "#endif\n");

    // QComm's Adreno driver doesn't seem to like using the framebuffer_fetch value as an
    // intermediate value with multiple reads & modifications, so we pull out the "real" output
    // value above and use a temporary for calculations, then set the output value once at the
    // end of the shader.
    out.Puts("\tfloat4 ocol0;\n");
  }

  if (uid_data->blend_enable)
  {
    out.Puts("\tfloat4 ocol1;\n");
  }

  if (host_config.backend_geometry_shaders && stereo)
  {
    if (host_config.backend_gl_layer_in_fs)
      out.Puts("\tint layer = gl_Layer;\n");
  }
  else
  {
    out.Puts("\tint layer = 0;\n");
  }

  out.Puts("\tDolphinFragmentInput frag_input;\n");
  out.Puts("\tfrag_input.color_0 = colors_0;\n");
  out.Puts("\tfrag_input.color_1 = colors_1;\n");
  out.Puts("\tfrag_input.layer = layer;\n");
  if (per_pixel_lighting)
  {
    out.Puts("\tfrag_input.normal = normalize(Normal);\n");
    out.Puts("\tfrag_input.position = WorldPos;\n");
  }
  else
  {
    out.Puts("\tfrag_input.normal = vec3(0, 0, 0);\n");
    out.Puts("\tfrag_input.position = vec3(0, 0, 0);\n");
  }
  for (u32 i = 0; i < uid_data->genMode_numtexgens; i++)
  {
    out.Printf("\tfrag_input.tex%u = tex%u;\n", i, i);
  }

  // Initialize other texture coordinates that are unused
  for (u32 i = uid_data->genMode_numtexgens; i < 8; i++)
  {
    out.Printf("\tfrag_input.tex%u = vec3(0, 0, 0);\n", i);
  }

  out.Puts("\tDolphinFragmentOutput frag_output;\n");
  out.Puts("\tprocess_fragment(frag_input, frag_output);\n");
  out.Puts("\tivec4 prev = frag_output.main & 255;\n");

  // NOTE: Fragment may not be discarded if alpha test always fails and early depth test is enabled
  // (in this case we need to write a depth value if depth test passes regardless of the alpha
  // testing result)
  if (uid_data->Pretest == AlphaTestResult::Undetermined ||
      (uid_data->Pretest == AlphaTestResult::Fail && uid_data->ztest == EmulatedZ::Late))
  {
    WriteAlphaTest(out, uid_data, api_type, uid_data->per_pixel_depth,
                   !uid_data->no_dual_src || uid_data->blend_enable);
  }

  // This situation is important for Mario Kart Wii's menus (they will render incorrectly if the
  // alpha test for the FMV in the background fails, since they depend on depth for drawing a yellow
  // border) and Fortune Street's gameplay (where a rectangle with an alpha value of 1 is drawn over
  // the center of the screen several times, but those rectangles shouldn't be visible).
  // Blending seems to result in no changes to the output with an alpha of 1, even if the input
  // color is white.
  // TODO: Investigate this further: we might be handling blending incorrectly in general (though
  // there might not be any good way of changing blending behavior)
  out.Puts("\t// Hardware testing indicates that an alpha of 1 can pass an alpha test,\n"
            "\t// but doesn't do anything in blending\n"
            "\tif (prev.a == 1) prev.a = 0;\n");

  if (uid_data->zfreeze)
  {
    out.SetConstantsUsed(C_ZSLOPE, C_ZSLOPE);
    out.SetConstantsUsed(C_EFBSCALE, C_EFBSCALE);

    out.Puts("\tfloat2 screenpos = rawpos.xy * "
              I_EFBSCALE
              ".xy;\n");

    // Opengl has reversed vertical screenspace coordinates
    if (api_type == APIType::OpenGL)
      out.Printf("\tscreenpos.y = %u.0 - screenpos.y;\n", EFB_HEIGHT);

    out.Puts("\tint zCoord = int("
              I_ZSLOPE
              ".z + "
              I_ZSLOPE
              ".x * screenpos.x + "
              I_ZSLOPE
              ".y * screenpos.y);\n");
  }
  else if (!host_config.fast_depth_calc)
  {
    // FastDepth means to trust the depth generated in perspective division.
    // It should be correct, but it seems not to be as accurate as required. TODO: Find out why!
    // For disabled FastDepth we just calculate the depth value again.
    // The performance impact of this additional calculation doesn't matter, but it prevents
    // the host GPU driver from performing any early depth test optimizations.
    out.SetConstantsUsed(C_ZBIAS + 1, C_ZBIAS + 1);
    // the screen space depth value = far z + (clip z / clip w) * z range
    out.Puts("\tint zCoord = "
              I_ZBIAS
              "[1].x + int((clipPos.z / clipPos.w) * float("
              I_ZBIAS
              "[1].y));\n");
  }
  else
  {
    if (!host_config.backend_reversed_depth_range)
      out.Puts("\tint zCoord = int((1.0 - rawpos.z) * 16777216.0);\n");
    else
      out.Puts("\tint zCoord = int(rawpos.z * 16777216.0);\n");
  }
  out.Puts("\tzCoord = clamp(zCoord, 0, 0xFFFFFF);\n");

  // depth texture can safely be ignored if the result won't be written to the depth buffer
  // (early_ztest) and isn't used for fog either
  const bool skip_ztexture = !uid_data->per_pixel_depth && uid_data->fog_fsel == FogType::Off;

  // Note: z-textures are not written to depth buffer if early depth test is used
  const bool early_ztest = uid_data->ztest == EmulatedZ::Early ||
                           uid_data->ztest == EmulatedZ::EarlyWithFBFetch ||
                           uid_data->ztest == EmulatedZ::EarlyWithZComplocHack;
  if (uid_data->per_pixel_depth && early_ztest)
  {
    if (!host_config.backend_reversed_depth_range)
      out.Puts("\tdepth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Puts("\tdepth = float(zCoord) / 16777216.0;\n");
  }

  // Note: depth texture output is only written to depth buffer if late depth test is used
  // theoretical final depth value is used for fog calculation, though, so we have to emulate
  // ztextures anyway
  if (uid_data->ztex_op != ZTexOp::Disabled && !skip_ztexture)
  {
    // use the texture input of the last texture stage, hopefully this has been read and
    // is in correct format...
    out.SetConstantsUsed(C_ZBIAS, C_ZBIAS + 1);
    out.Printf("\tzCoord = idot("
              I_ZBIAS
              "[0].xyzw, frag_output.last_texture.xyzw) + "
              I_ZBIAS
              "[1].w %s;\n", (uid_data->ztex_op == ZTexOp::Add) ? "+ zCoord" : "");
    out.Puts("\tzCoord = zCoord & 0xFFFFFF;\n");
  }

  if (uid_data->per_pixel_depth && uid_data->ztest == EmulatedZ::Late)
  {
    if (!host_config.backend_reversed_depth_range)
      out.Puts("\tdepth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Puts("\tdepth = float(zCoord) / 16777216.0;\n");
  }

  // No dithering for RGB8 mode
  if (uid_data->dither)
  {
    // Flipper uses a standard 2x2 Bayer Matrix for 6 bit dithering
    out.Puts("\tint2 dither = int2(rawpos.xy) & 1;\n");
    out.Puts("\tprev.rgb = (prev.rgb - (prev.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;\n");
  }

  WriteFog(out, uid_data);

  if (uid_data->logic_op_enable)
    WriteLogicOp(out, uid_data);
  else if (uid_data->emulate_logic_op_with_blend)
    WriteLogicOpBlend(out, uid_data);

  // Write the color and alpha values to the framebuffer
  // If using shader blend, we still use the separate alpha
  const bool use_dual_source = !uid_data->no_dual_src || uid_data->blend_enable;
  WriteColor(out, api_type, uid_data, use_dual_source);

  if (uid_data->blend_enable)
    WriteBlend(out, uid_data);
  else if (use_framebuffer_fetch)
    out.Puts("\treal_ocol0 = ocol0;\n");

  if (uid_data->bounding_box)
    out.Puts("\tUpdateBoundingBox(rawpos.xy);\n");

  out.Puts("}\n");

  return out;
}

static void WriteStage(ShaderCode& out, const pixel_shader_uid_data* uid_data, int n,
                       APIType api_type, bool stereo)
{
  using Common::EnumMap;

  const auto& stage = uid_data->stagehash[n];
  out.Printf("\n"
            "\t// TEV stage %d\n", n);

  // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord does not
  // exist), then tex coord 0 is used (though sometimes glitchy effects happen on console).
  u32 texcoord = stage.tevorders_texcoord;
  const bool has_tex_coord = texcoord < uid_data->genMode_numtexgens;
  if (!has_tex_coord)
    texcoord = 0;

  {
    const TevStageIndirect tevind{.hex = stage.tevind};
    out.Puts("\t// indirect op\n");

    // Quirk: Referencing a stage above the number of ind stages is undefined behavior,
    // and on console produces a noise pattern (details unknown).
    // Instead, just skip applying the indirect operation, which is close enough.
    // We need to do *something*, as there won't be an iindtex variable otherwise.
    // Viewtiful Joe hits this case (bug 12525).
    // Wrapping and add to previous still apply in this case (and when the stage is disabled).
    const bool has_ind_stage = tevind.bt < uid_data->genMode_numindstages;

    // Perform the indirect op on the incoming regular coordinates
    // using iindtex{} as the offset coords
    if (has_ind_stage && tevind.bs != IndTexBumpAlpha::Off)
    {
      static constexpr EnumMap<const char*, IndTexBumpAlpha::U> tev_ind_alpha_sel{
          "",
          "x",
          "y",
          "z",
      };

      // According to libogc, the bump alpha value is 5 bits, and comes from the bottom bits of the
      // component byte, except in the case of ITF_8, which presumably uses the top bits with a
      // mask.
      // https://github.com/devkitPro/libogc/blob/bd24a9b3f59502f9b30d6bac0ae35fc485045f78/gc/ogc/gx.h#L3038-L3041
      // https://github.com/devkitPro/libogc/blob/bd24a9b3f59502f9b30d6bac0ae35fc485045f78/gc/ogc/gx.h#L790-L800

      static constexpr EnumMap<char, IndTexFormat::ITF_3> tev_ind_alpha_shift{
          '0',  // ITF_8: 0bXXXXXYYY -> 0bXXXXX000? No shift?
          '5',  // ITF_5: 0bIIIIIAAA -> 0bAAA00000, shift of 5
          '4',  // ITF_4: 0bIIIIAAAA -> 0bAAAA0000, shift of 4
          '3',  // ITF_3: 0bIIIAAAAA -> 0bAAAAA000, shift of 3
      };

      out.Printf("\talphabump = (iindtex%u.%s << %c) & 248;\n", static_cast<u32>(tevind.bt),
                 tev_ind_alpha_sel[tevind.bs], tev_ind_alpha_shift[tevind.fmt]);
    }
    else
    {
      // TODO: Should we reset alphabump to 0 here?
    }

    if (has_ind_stage && tevind.matrix_index != IndMtxIndex::Off)
    {
      // format
      static constexpr EnumMap<char, IndTexFormat::ITF_3> tev_ind_fmt_shift{
          '0',  // ITF_8: 0bXXXXXXXX -> 0bXXXXXXXX, no shift
          '3',  // ITF_5: 0bIIIIIAAA -> 0b000IIIII, shift of 3
          '4',  // ITF_4: 0bIIIIAAAA -> 0b0000IIII, shift of 4
          '5',  // ITF_3: 0bIIIAAAAA -> 0b00000III, shift of 5
      };
      out.Printf("\tint3 iindtevcrd%d = iindtex%u >> %c;\n", n, static_cast<u32>(tevind.bt),
                 tev_ind_fmt_shift[tevind.fmt]);

      // bias - TODO: Check if this needs to be this complicated...
      // indexed by bias
      static constexpr EnumMap<const char*, IndTexBias::STU> tev_ind_bias_field{
          "", "x", "y", "xy", "z", "xz", "yz", "xyz",
      };

      // indexed by fmt
      static constexpr EnumMap<const char*, IndTexFormat::ITF_3> tev_ind_bias_add{
          "-128",
          "1",
          "1",
          "1",
      };

      if (tevind.bias == IndTexBias::S || tevind.bias == IndTexBias::T ||
          tevind.bias == IndTexBias::U)
      {
        out.Printf("\tiindtevcrd%d.%s += int(%s);\n", n, tev_ind_bias_field[tevind.bias], tev_ind_bias_add[tevind.fmt]);
      }
      else if (tevind.bias == IndTexBias::ST || tevind.bias == IndTexBias::SU ||
               tevind.bias == IndTexBias::TU_)
      {
        out.Printf("\tiindtevcrd%d.%s += int2(%s, %s);\n", n, tev_ind_bias_field[tevind.bias], tev_ind_bias_add[tevind.fmt], tev_ind_bias_add[tevind.fmt]);
      }
      else if (tevind.bias == IndTexBias::STU)
      {
        out.Printf("\tiindtevcrd%d.%s += int3(%s, %s, %s);\n", n, tev_ind_bias_field[tevind.bias], tev_ind_bias_add[tevind.fmt], tev_ind_bias_add[tevind.fmt], tev_ind_bias_add[tevind.fmt]);
      }

      // Multiplied by 2 because each matrix has two rows.
      // Note also that the 4th column of the matrix contains the scale factor.
      const u32 mtxidx = 2 * (static_cast<u32>(tevind.matrix_index.Value()) - 1);

      // multiply by offset matrix and scale - calculations are likely to overflow badly,
      // yet it works out since we only care about the lower 23 bits (+1 sign bit) of the result
      if (tevind.matrix_id == IndMtxId::Indirect)
      {
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Printf("\tint2 indtevtrans%d = int2(idot("
                  I_INDTEXMTX
                  "[%u].xyz, iindtevcrd%d), idot("
                  I_INDTEXMTX
                  "[%u].xyz, iindtevcrd%d)) >> 3;\n", n, mtxidx, n, mtxidx + 1, n);

        // TODO: should use a shader uid branch for this for better performance
        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Printf("\tint indtexmtx_w_inverse_%d = -"
                    I_INDTEXMTX
                    "[%u].w;\n", n, mtxidx);
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= indtexmtx_w_inverse_%d;\n", n, n);
        }
        else
        {
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= (-"
                    I_INDTEXMTX
                    "[%u].w);\n", n, mtxidx);
        }
      }
      else if (tevind.matrix_id == IndMtxId::S)
      {
        ASSERT(has_tex_coord);
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Printf("\tint2 indtevtrans%d = int2(fixpoint_uv%u * iindtevcrd%d.xx) >> 8;\n", n, texcoord, n);
        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Printf("\tint  indtexmtx_w_inverse_%d = -"
                    I_INDTEXMTX
                    "[%u].w;\n", n, mtxidx);
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= (indtexmtx_w_inverse_%d);\n", n, n);
        }
        else
        {
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= (-"
                    I_INDTEXMTX
                    "[%u].w);\n", n, mtxidx);
        }
      }
      else if (tevind.matrix_id == IndMtxId::T)
      {
        ASSERT(has_tex_coord);
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Printf("\tint2 indtevtrans%d = int2(fixpoint_uv%u * iindtevcrd%d.yy) >> 8;\n", n, texcoord, n);

        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Printf("\tint  indtexmtx_w_inverse_%d = -"
                    I_INDTEXMTX
                    "[%u].w;\n", n, mtxidx);
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= (indtexmtx_w_inverse_%d);\n", n, n);
        }
        else
        {
          out.Printf("\tif ("
                    I_INDTEXMTX
                    "[%u].w >= 0) indtevtrans%d >>= "
                    I_INDTEXMTX
                    "[%u].w;\n", mtxidx, n, mtxidx);
          out.Printf("\telse indtevtrans%d <<= (-"
                    I_INDTEXMTX
                    "[%u].w);\n", n, mtxidx);
        }
      }
      else
      {
        out.Printf("\tint2 indtevtrans%d = int2(0, 0);\n", n);
        ASSERT(false);  // Unknown value for matrix_id
      }
    }
    else
    {
      out.Printf("\tint2 indtevtrans%d = int2(0, 0);\n", n);
      if (tevind.matrix_index == IndMtxIndex::Off)
      {
        // If matrix_index is Off (0), matrix_id should be Indirect (0)
        ASSERT(tevind.matrix_id == IndMtxId::Indirect);
      }
    }

    // ---------
    // Wrapping
    // ---------

    static constexpr std::array<const char*, 5> tev_ind_wrap_start{
        "(256<<7)", "(128<<7)", "(64<<7)", "(32<<7)", "(16<<7)",
    };

    // wrap S
    if (tevind.sw == IndTexWrap::ITW_OFF)
    {
      out.Printf("\twrappedcoord.x = fixpoint_uv%u.x;\n", texcoord);
    }
    else if (tevind.sw >= IndTexWrap::ITW_0)  // 7 (Invalid) appears to behave the same as 6 (ITW_0)
    {
      out.Puts("\twrappedcoord.x = 0;\n");
    }
    else
    {
      out.Printf("\twrappedcoord.x = fixpoint_uv%u.x & (%s - 1);\n", texcoord, tev_ind_wrap_start[u32(tevind.sw.Value()) - u32(IndTexWrap::ITW_256)]);
    }

    // wrap T
    if (tevind.tw == IndTexWrap::ITW_OFF)
    {
      out.Printf("\twrappedcoord.y = fixpoint_uv%u.y;\n", texcoord);
    }
    else if (tevind.tw >= IndTexWrap::ITW_0)  // 7 (Invalid) appears to behave the same as 6 (ITW_0)
    {
      out.Puts("\twrappedcoord.y = 0;\n");
    }
    else
    {
      out.Printf("\twrappedcoord.y = fixpoint_uv%u.y & (%s - 1);\n", texcoord, tev_ind_wrap_start[u32(tevind.tw.Value()) - u32(IndTexWrap::ITW_256)]);
    }

    if (tevind.fb_addprev)  // add previous tevcoord
      out.Printf("\ttevcoord.xy += wrappedcoord + indtevtrans%d;\n", n);
    else
      out.Printf("\ttevcoord.xy = wrappedcoord + indtevtrans%d;\n", n);

    // Emulate s24 overflows
    out.Puts("\ttevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
  }

  TevStageCombiner::ColorCombiner cc;
  TevStageCombiner::AlphaCombiner ac;
  cc.hex = stage.cc;
  ac.hex = stage.ac;

  if (cc.a == TevColorArg::RasAlpha || cc.a == TevColorArg::RasColor ||
      cc.b == TevColorArg::RasAlpha || cc.b == TevColorArg::RasColor ||
      cc.c == TevColorArg::RasAlpha || cc.c == TevColorArg::RasColor ||
      cc.d == TevColorArg::RasAlpha || cc.d == TevColorArg::RasColor ||
      ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
      ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha)
  {
    // Generate swizzle string to represent the Ras color channel swapping
    out.Printf("\trastemp = %s.%c%c%c%c;\n", tev_ras_table[stage.tevorders_colorchan], rgba_swizzle[stage.ras_swap_r], rgba_swizzle[stage.ras_swap_g], rgba_swizzle[stage.ras_swap_b], rgba_swizzle[stage.ras_swap_a]);
  }

  if (stage.tevorders_enable && uid_data->genMode_numtexgens > 0)
  {
    // Generate swizzle string to represent the texture color channel swapping
    out.Printf("\trawtextemp = sampleTextureWrapper(%du, tevcoord.xy, layer);\n", stage.tevorders_texmap);
    out.Printf("\ttextemp = rawtextemp.%c%c%c%c;\n", rgba_swizzle[stage.tex_swap_r], rgba_swizzle[stage.tex_swap_g], rgba_swizzle[stage.tex_swap_b], rgba_swizzle[stage.tex_swap_a]);
  }
  else if (uid_data->genMode_numtexgens == 0)
  {
    // It seems like the result is always black when no tex coords are enabled, but further testing
    // is needed.
    out.Puts("\ttextemp = int4(0, 0, 0, 0);\n");
  }
  else
  {
    out.Puts("\ttextemp = int4(255, 255, 255, 255);\n");
  }

  if (cc.a == TevColorArg::Konst || cc.b == TevColorArg::Konst || cc.c == TevColorArg::Konst ||
      cc.d == TevColorArg::Konst || ac.a == TevAlphaArg::Konst || ac.b == TevAlphaArg::Konst ||
      ac.c == TevAlphaArg::Konst || ac.d == TevAlphaArg::Konst)
  {
    out.Printf("\tkonsttemp = int4(%s, %s);\n", tev_ksel_table_c[stage.tevksel_kc], tev_ksel_table_a[stage.tevksel_ka]);

    if (u32(stage.tevksel_kc) > 7)
    {
      out.SetConstantsUsed(C_KCOLORS + ((u32(stage.tevksel_kc) - 0xc) % 4),
                           C_KCOLORS + ((u32(stage.tevksel_kc) - 0xc) % 4));
    }
    if (u32(stage.tevksel_ka) > 7)
    {
      out.SetConstantsUsed(C_KCOLORS + ((u32(stage.tevksel_ka) - 0xc) % 4),
                           C_KCOLORS + ((u32(stage.tevksel_ka) - 0xc) % 4));
    }
  }

  if (cc.d == TevColorArg::Color0 || cc.d == TevColorArg::Alpha0 || ac.d == TevAlphaArg::Alpha0)
    out.SetConstantsUsed(C_COLORS + 1, C_COLORS + 1);

  if (cc.d == TevColorArg::Color1 || cc.d == TevColorArg::Alpha1 || ac.d == TevAlphaArg::Alpha1)
    out.SetConstantsUsed(C_COLORS + 2, C_COLORS + 2);

  if (cc.d == TevColorArg::Color2 || cc.d == TevColorArg::Alpha2 || ac.d == TevAlphaArg::Alpha2)
    out.SetConstantsUsed(C_COLORS + 3, C_COLORS + 3);

  if (cc.dest >= TevOutput::Color0)
    out.SetConstantsUsed(C_COLORS + u32(cc.dest.Value()), C_COLORS + u32(cc.dest.Value()));

  if (ac.dest >= TevOutput::Color0)
    out.SetConstantsUsed(C_COLORS + u32(ac.dest.Value()), C_COLORS + u32(ac.dest.Value()));

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VECTOR_BITWISE_AND))
  {
    out.Printf("\ttevin_a = int4(%s & 255, %s & 255);\n", tev_c_input_table[cc.a], tev_a_input_table[ac.a]);
    out.Printf("\ttevin_b = int4(%s & 255, %s & 255);\n", tev_c_input_table[cc.b], tev_a_input_table[ac.b]);
    out.Printf("\ttevin_c = int4(%s & 255, %s & 255);\n", tev_c_input_table[cc.c], tev_a_input_table[ac.c]);
  }
  else
  {
    out.Printf("\ttevin_a = int4(%s, %s)&int4(255, 255, 255, 255);\n", tev_c_input_table[cc.a], tev_a_input_table[ac.a]);
    out.Printf("\ttevin_b = int4(%s, %s)&int4(255, 255, 255, 255);\n", tev_c_input_table[cc.b], tev_a_input_table[ac.b]);
    out.Printf("\ttevin_c = int4(%s, %s)&int4(255, 255, 255, 255);\n", tev_c_input_table[cc.c], tev_a_input_table[ac.c]);
  }
  out.Printf("\ttevin_d = int4(%s, %s);\n", tev_c_input_table[cc.d], tev_a_input_table[ac.d]);

  out.Puts("\t// color combine\n");
  out.Printf("\t%s = clamp(", tev_c_output_table[cc.dest]);
  if (cc.bias != TevBias::Compare)
  {
    WriteTevRegular(out, "rgb", cc.bias, cc.op, cc.clamp, cc.scale);
  }
  else
  {
    static constexpr EnumMap<const char*, TevCompareMode::RGB8> tev_rgb_comparison_gt{
        "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",  // TevCompareMode::R8
        "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : int3(0,0,0))",  // GR16
        "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : int3(0,0,0))",  // BGR24
        "(max(sign(tevin_a.rgb - tevin_b.rgb), int3(0,0,0)) * tevin_c.rgb)",  // RGB8
    };

    static constexpr EnumMap<const char*, TevCompareMode::RGB8> tev_rgb_comparison_eq{
        "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",  // TevCompareMode::R8
        "((idot(tevin_a.rgb,comp16) == idot(tevin_b.rgb,comp16)) ? tevin_c.rgb : int3(0,0,0))",  // GR16
        "((idot(tevin_a.rgb,comp24) == idot(tevin_b.rgb,comp24)) ? tevin_c.rgb : int3(0,0,0))",  // BGR24
        "((int3(1,1,1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)"  // RGB8
    };

    if (cc.comparison == TevComparison::EQ)
      out.Printf("   tevin_d.rgb + %s", tev_rgb_comparison_eq[cc.compare_mode]);
    else
      out.Printf("   tevin_d.rgb + %s", tev_rgb_comparison_gt[cc.compare_mode]);
  }
  if (cc.clamp)
    out.Puts(", int3(0,0,0), int3(255,255,255))");
  else
    out.Puts(", int3(-1024,-1024,-1024), int3(1023,1023,1023))");
  out.Puts(";\n");

  out.Puts("\t// alpha combine\n");
  out.Printf("\t%s = clamp(", tev_a_output_table[ac.dest]);
  if (ac.bias != TevBias::Compare)
  {
    WriteTevRegular(out, "a", ac.bias, ac.op, ac.clamp, ac.scale);
  }
  else
  {
    static constexpr EnumMap<const char*, TevCompareMode::A8> tev_a_comparison_gt{
        "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",  // TevCompareMode::R8
        "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",  // GR16
        "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",  // BGR24
        "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)",                                  // A8
    };

    static constexpr EnumMap<const char*, TevCompareMode::A8> tev_a_comparison_eq{
        "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",  // TevCompareMode::R8
        "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",  // GR16,
        "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",  // BGR24,
        "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)",                                  // A8
    };

    if (ac.comparison == TevComparison::EQ)
      out.Printf("   tevin_d.a + %s", tev_a_comparison_eq[ac.compare_mode]);
    else
      out.Printf("   tevin_d.a + %s", tev_a_comparison_gt[ac.compare_mode]);
  }
  if (ac.clamp)
    out.Puts(", 0, 255)");
  else
    out.Puts(", -1024, 1023)");

  out.Puts(";\n");
}

static void WriteTevRegular(ShaderCode& out, const char* components, TevBias bias, TevOp op,
                            bool clamp, TevScale scale)
{
  static constexpr Common::EnumMap<const char*, TevScale::Divide2> tev_scale_table_left{
      "",       // Scale1
      " << 1",  // Scale2
      " << 2",  // Scale4
      "",       // Divide2
  };

  static constexpr Common::EnumMap<const char*, TevScale::Divide2> tev_scale_table_right{
      "",       // Scale1
      "",       // Scale2
      "",       // Scale4
      " >> 1",  // Divide2
  };

  static constexpr Common::EnumMap<const char*, TevOp::Sub> tev_lerp_bias{
      " + 128",
      " + 127",
  };

  static constexpr Common::EnumMap<const char*, TevBias::Compare> tev_bias_table{
      "",        // Zero,
      " + 128",  // AddHalf,
      " - 128",  // SubHalf,
      "",
  };

  static constexpr Common::EnumMap<char, TevOp::Sub> tev_op_table{
      '+',  // TevOp::Add = 0,
      '-',  // TevOp::Sub = 1,
  };

  // Regular TEV stage: (d + bias + lerp(a,b,c)) * scale
  // The GameCube/Wii GPU uses a very sophisticated algorithm for scale-lerping:
  // - c is scaled from 0..255 to 0..256, which allows dividing the result by 256 instead of 255
  // - if scale is bigger than one, it is moved inside the lerp calculation for increased accuracy
  // - a rounding bias is added before dividing by 256
  // TODO: Is the rounding bias still added when the scale is divide by 2?  Currently we do not
  // apply it.
  out.Printf("(((tevin_d.%s%s)%s)", components, tev_bias_table[bias], tev_scale_table_left[scale]);
  out.Printf(" %c ", tev_op_table[op]);
  out.Printf("(((((tevin_a.%s<<8) + (tevin_b.%s-tevin_a.%s)*(tevin_c.%s+(tevin_c.%s>>7)))%s)%s)>>8)", components, components, components, components, components, tev_scale_table_left[scale], (scale != TevScale::Divide2) ? tev_lerp_bias[op] : "");
  out.Printf(")%s", tev_scale_table_right[scale]);
}

constexpr Common::EnumMap<const char*, CompareMode::Always> tev_alpha_funcs_table{
    "(false)",         // CompareMode::Never
    "(prev.a <  %s)",  // CompareMode::Less
    "(prev.a == %s)",  // CompareMode::Equal
    "(prev.a <= %s)",  // CompareMode::LEqual
    "(prev.a >  %s)",  // CompareMode::Greater
    "(prev.a != %s)",  // CompareMode::NEqual
    "(prev.a >= %s)",  // CompareMode::GEqual
    "(true)"           // CompareMode::Always
};

constexpr Common::EnumMap<const char*, AlphaTestOp::Xnor> tev_alpha_funclogic_table{
    " && ",  // and
    " || ",  // or
    " != ",  // xor
    " == "   // xnor
};

static void WriteAlphaTest(ShaderCode& out, const pixel_shader_uid_data* uid_data, APIType api_type,
                           bool per_pixel_depth, bool use_dual_source)
{
  static constexpr const char* alpha_ref[2]{
      I_ALPHA ".r",
      I_ALPHA ".g",
  };

  const auto write_alpha_func = [&out](CompareMode mode, const char* ref) {
    const bool has_no_arguments = mode == CompareMode::Never || mode == CompareMode::Always;
    if (has_no_arguments)
      out.Puts(tev_alpha_funcs_table[mode]);
    else
      out.Printf(tev_alpha_funcs_table[mode], ref);
  };

  out.SetConstantsUsed(C_ALPHA, C_ALPHA);

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN))
    out.Puts("\tif(( ");
  else
    out.Puts("\tif(!( ");

  // Lookup the first component from the alpha function table
  write_alpha_func(uid_data->alpha_test_comp0, alpha_ref[0]);

  // Lookup the logic op
  out.Puts(tev_alpha_funclogic_table[uid_data->alpha_test_logic]);

  // Lookup the second component from the alpha function table
  write_alpha_func(uid_data->alpha_test_comp1, alpha_ref[1]);

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN))
    out.Puts(") == false) {\n");
  else
    out.Puts(")) {\n");

  if (uid_data->uint_output)
    out.Puts("\t\tocol0 = uint4(0, 0, 0, 0);\n");
  else
    out.Puts("\t\tocol0 = float4(0.0, 0.0, 0.0, 0.0);\n");
  if (use_dual_source)
  {
    if (uid_data->uint_output)
      out.Puts("\t\tocol1 = uint4(0, 0, 0, 0);\n");
    else
      out.Puts("\t\tocol1 = float4(0.0, 0.0, 0.0, 0.0);\n");
  }
  if (per_pixel_depth)
  {
    out.Printf("\t\tdepth = %s;\n", !g_backend_info.bSupportsReversedDepthRange ? "0.0" : "1.0");
  }

  // ZCOMPLOC HACK:
  if (uid_data->ztest != EmulatedZ::EarlyWithZComplocHack)
  {
#ifdef __APPLE__
    if (uid_data->ztest == EmulatedZ::EarlyWithFBFetch)
    {
      // Instead of using discard, fetch the framebuffer's color value and use it as the output
      // for this fragment.
      out.Printf("\t\t%s = float4(initial_ocol0.xyz, 1.0);\n", use_dual_source ? "real_ocol0" : "ocol0");
      out.Puts("\t\treturn;\n");
    }
    else
#endif
    {
      out.Puts("\t\tdiscard;\n");
      if (api_type == APIType::D3D)
        out.Puts("\t\treturn;\n");
    }
  }

  out.Puts("\t}\n");
}

constexpr Common::EnumMap<const char*, FogType::BackwardsExpSq> tev_fog_funcs_table{
    "",                                                       // No Fog
    "",                                                       // ?
    "",                                                       // Linear
    "",                                                       // ?
    "\tfog = 1.0 - exp2(-8.0 * fog);\n",                      // exp
    "\tfog = 1.0 - exp2(-8.0 * fog * fog);\n",                // exp2
    "\tfog = exp2(-8.0 * (1.0 - fog));\n",                    // backward exp
    "\tfog = 1.0 - fog;\n   fog = exp2(-8.0 * fog * fog);\n"  // backward exp2
};

static void WriteFog(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  if (uid_data->fog_fsel == FogType::Off)
    return;  // no Fog

  out.SetConstantsUsed(C_FOGCOLOR, C_FOGCOLOR);
  out.SetConstantsUsed(C_FOGI, C_FOGI);
  out.SetConstantsUsed(C_FOGF, C_FOGF + 1);
  if (uid_data->fog_proj == FogProjection::Perspective)
  {
    // perspective
    // ze = A/(B - (Zs >> B_SHF)
    // TODO: Verify that we want to drop lower bits here! (currently taken over from software
    // renderer)
    //       Maybe we want to use "ze = (A << B_SHF)/((B << B_SHF) - Zs)" instead?
    //       That's equivalent, but keeps the lower bits of Zs.
    out.Puts("\tfloat ze = ("
              I_FOGF
              ".x * 16777216.0) / float("
              I_FOGI
              ".y - (zCoord >> "
              I_FOGI
              ".w));\n");
  }
  else
  {
    // orthographic
    // ze = a*Zs    (here, no B_SHF)
    out.Puts("\tfloat ze = "
              I_FOGF
              ".x * float(zCoord) / 16777216.0;\n");
  }

  // x_adjust = sqrt((x-center)^2 + k^2)/k
  // ze *= x_adjust
  if (uid_data->fog_RangeBaseEnabled)
  {
    out.SetConstantsUsed(C_FOGF, C_FOGF);
    out.Puts("\tfloat offset = (2.0 * (rawpos.x / "
              I_FOGF
              ".w)) - 1.0 - "
              I_FOGF
              ".z;\n"
              "\tfloat floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
              "\tuint indexlower = uint(floatindex);\n"
              "\tuint indexupper = indexlower + 1u;\n"
              "\tfloat klower = "
              I_FOGRANGE
              "[indexlower >> 2u][indexlower & 3u];\n"
              "\tfloat kupper = "
              I_FOGRANGE
              "[indexupper >> 2u][indexupper & 3u];\n"
              "\tfloat k = lerp(klower, kupper, frac(floatindex));\n"
              "\tfloat x_adjust = sqrt(offset * offset + k * k) / k;\n"
              "\tze *= x_adjust;\n");
  }

  out.Puts("\tfloat fog = clamp(ze - "
            I_FOGF
            ".y, 0.0, 1.0);\n");

  if (uid_data->fog_fsel >= FogType::Exp)
  {
    out.Puts(tev_fog_funcs_table[uid_data->fog_fsel]);
  }
  else
  {
    if (uid_data->fog_fsel != FogType::Linear)
      WARN_LOG_FMT(VIDEO, "Unknown Fog Type! {}", uid_data->fog_fsel);
  }

  out.Puts("\tint ifog = iround(fog * 256.0);\n");
  out.Puts("\tprev.rgb = (prev.rgb * (256 - ifog) + "
            I_FOGCOLOR
            ".rgb * ifog) >> 8;\n");
}

static void WriteLogicOp(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  static constexpr std::array<const char*, 16> logic_op_mode{
      "int4(0, 0, 0, 0)",          // CLEAR
      "prev & fb_value",           // AND
      "prev & ~fb_value",          // AND_REVERSE
      "prev",                      // COPY
      "~prev & fb_value",          // AND_INVERTED
      "fb_value",                  // NOOP
      "prev ^ fb_value",           // XOR
      "prev | fb_value",           // OR
      "~(prev | fb_value)",        // NOR
      "~(prev ^ fb_value)",        // EQUIV
      "~fb_value",                 // INVERT
      "prev | ~fb_value",          // OR_REVERSE
      "~prev",                     // COPY_INVERTED
      "~prev | fb_value",          // OR_INVERTED
      "~(prev & fb_value)",        // NAND
      "int4(255, 255, 255, 255)",  // SET
  };

  out.Puts("\tint4 fb_value = iround(initial_ocol0 * 255.0);\n");
  out.Printf("\tprev = (%s) & 0xff;\n", logic_op_mode[uid_data->logic_op_mode]);
}

static void WriteLogicOpBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  switch (static_cast<LogicOp>(uid_data->logic_op_mode))
  {
  case LogicOp::Clear:
  case LogicOp::NoOp:
    out.Puts("\tprev = int4(0, 0, 0, 0);\n");
    break;
  case LogicOp::Copy:
    // Do nothing!
    break;
  case LogicOp::CopyInverted:
    out.Puts("\tprev ^= 255;\n");
    break;
  case LogicOp::Set:
  case LogicOp::Invert:  // In cooperation with blend
    out.Puts("\tprev = int4(255, 255, 255, 255);\n");
    break;
  default:
    break;
  }
}

static void WriteColor(ShaderCode& out, APIType api_type, const pixel_shader_uid_data* uid_data,
                       bool use_dual_source)
{
  // Some backends require the shader outputs be uint when writing to a uint render target for logic
  // op.
  if (uid_data->uint_output)
  {
    if (uid_data->rgba6_format)
      out.Puts("\tocol0 = uint4(prev & 0xFC);\n");
    else
      out.Puts("\tocol0 = uint4(prev);\n");
    return;
  }

  if (uid_data->rgba6_format)
    out.Puts("\tocol0.rgb = float3(prev.rgb >> 2) / 63.0;\n");
  else
    out.Puts("\tocol0.rgb = float3(prev.rgb) / 255.0;\n");

  // Colors will be blended against the 8-bit alpha from ocol1 and
  // the 6-bit alpha from ocol0 will be written to the framebuffer
  if (uid_data->useDstAlpha)
  {
    out.SetConstantsUsed(C_ALPHA, C_ALPHA);
    out.Puts("\tocol0.a = float("
              I_ALPHA
              ".a >> 2) / 63.0;\n");

    // Use dual-source color blending to perform dst alpha in a single pass
    if (use_dual_source)
      out.Puts("\tocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
  }
  else
  {
    out.Puts("\tocol0.a = float(prev.a >> 2) / 63.0;\n");
    if (use_dual_source)
      out.Puts("\tocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
  }
}

static void WriteBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  if (uid_data->blend_enable)
  {
    using Common::EnumMap;
    static constexpr EnumMap<const char*, SrcBlendFactor::InvDstAlpha> blend_src_factor{
        "float3(0,0,0);",                      // ZERO
        "float3(1,1,1);",                      // ONE
        "initial_ocol0.rgb;",                  // DSTCLR
        "float3(1,1,1) - initial_ocol0.rgb;",  // INVDSTCLR
        "src_color.aaa;",                      // SRCALPHA
        "float3(1,1,1) - src_color.aaa;",      // INVSRCALPHA
        "initial_ocol0.aaa;",                  // DSTALPHA
        "float3(1,1,1) - initial_ocol0.aaa;",  // INVDSTALPHA
    };
    static constexpr EnumMap<const char*, SrcBlendFactor::InvDstAlpha> blend_src_factor_alpha{
        "0.0;",                    // ZERO
        "1.0;",                    // ONE
        "initial_ocol0.a;",        // DSTCLR
        "1.0 - initial_ocol0.a;",  // INVDSTCLR
        "src_color.a;",            // SRCALPHA
        "1.0 - src_color.a;",      // INVSRCALPHA
        "initial_ocol0.a;",        // DSTALPHA
        "1.0 - initial_ocol0.a;",  // INVDSTALPHA
    };
    static constexpr EnumMap<const char*, DstBlendFactor::InvDstAlpha> blend_dst_factor{
        "float3(0,0,0);",                      // ZERO
        "float3(1,1,1);",                      // ONE
        "ocol0.rgb;",                          // SRCCLR
        "float3(1,1,1) - ocol0.rgb;",          // INVSRCCLR
        "src_color.aaa;",                      // SRCALHA
        "float3(1,1,1) - src_color.aaa;",      // INVSRCALPHA
        "initial_ocol0.aaa;",                  // DSTALPHA
        "float3(1,1,1) - initial_ocol0.aaa;",  // INVDSTALPHA
    };
    static constexpr EnumMap<const char*, DstBlendFactor::InvDstAlpha> blend_dst_factor_alpha{
        "0.0;",                    // ZERO
        "1.0;",                    // ONE
        "ocol0.a;",                // SRCCLR
        "1.0 - ocol0.a;",          // INVSRCCLR
        "src_color.a;",            // SRCALPHA
        "1.0 - src_color.a;",      // INVSRCALPHA
        "initial_ocol0.a;",        // DSTALPHA
        "1.0 - initial_ocol0.a;",  // INVDSTALPHA
    };
    out.Printf("\tfloat4 src_color = %s;\n"
              "\tfloat4 blend_src;", uid_data->useDstAlpha ? "ocol1" : "ocol0");
    out.Printf("\tblend_src.rgb = %s\n", blend_src_factor[uid_data->blend_src_factor]);
    out.Printf("\tblend_src.a = %s\n", blend_src_factor_alpha[uid_data->blend_src_factor_alpha]);
    out.Puts("\tfloat4 blend_dst;\n");
    out.Printf("\tblend_dst.rgb = %s\n", blend_dst_factor[uid_data->blend_dst_factor]);
    out.Printf("\tblend_dst.a = %s\n", blend_dst_factor_alpha[uid_data->blend_dst_factor_alpha]);

    out.Puts("\tfloat4 blend_result;\n");
    if (uid_data->blend_subtract)
    {
      out.Puts("\tblend_result.rgb = initial_ocol0.rgb * blend_dst.rgb - ocol0.rgb * blend_src.rgb;\n");
    }
    else
    {
      out.Puts("\tblend_result.rgb = initial_ocol0.rgb * blend_dst.rgb + ocol0.rgb * blend_src.rgb;\n");
    }

    if (uid_data->blend_subtract_alpha)
      out.Puts("\tblend_result.a = initial_ocol0.a * blend_dst.a - ocol0.a * blend_src.a;\n");
    else
      out.Puts("\tblend_result.a = initial_ocol0.a * blend_dst.a + ocol0.a * blend_src.a;\n");
  }
  else
  {
    out.Puts("\tfloat4 blend_result = ocol0;\n");
  }

  out.Puts("\treal_ocol0 = blend_result;\n");
}

void WriteFragmentBody(APIType api_type, const ShaderHostConfig& host_config,
                       const pixel_shader_uid_data* uid_data, ShaderCode& out)
{
  const bool per_pixel_lighting = host_config.per_pixel_lighting;
  const bool stereo = host_config.stereo;
  const u32 numStages = uid_data->genMode_numtevstages + 1;

  out.Puts("\tvec4 col0 = frag_input.color_0;\n");
  out.Puts("\tvec4 col1 = frag_input.color_1;\n");
  out.Puts("\tint layer = frag_input.layer;\n");

  out.Puts("\tint4 c0 = "
            I_COLORS
            "[1], c1 = "
            I_COLORS
            "[2], c2 = "
            I_COLORS
            "[3], prev = "
            I_COLORS
            "[0];\n"
            "\tint4 rastemp = int4(0, 0, 0, 0), rawtextemp = int4(0, 0, 0, 0), textemp = int4(0, 0, 0, 0), konsttemp = int4(0, 0, 0, 0);\n"
            "\tint3 comp16 = int3(1, 256, 0), comp24 = int3(1, 256, 256*256);\n"
            "\tint alphabump=0;\n"
            "\tint3 tevcoord=int3(0, 0, 0);\n"
            "\tint2 wrappedcoord=int2(0,0), tempcoord=int2(0,0);\n"
            "\tint4 tevin_a=int4(0,0,0,0),tevin_b=int4(0,0,0,0),tevin_c=int4(0,0,0,0),tevin_d=int4(0,0,0,0);\n"
            "\n");  // tev combiner inputs

  if (per_pixel_lighting)
  {
    if (uid_data->numColorChans > 0)
    {
      out.Puts("\tcol0 = dolphin_calculate_lighting_chn0(col0, frag_input.position, frag_input.normal);\n");
    }
    else
    {
      // The number of colors available to TEV is determined by numColorChans.
      // We have to provide the fields to match the interface, so set to zero if it's not enabled.
      out.Puts("\tcol0 = vec4(0.0, 0.0, 0.0, 0.0);\n");
    }

    if (uid_data->numColorChans == 2)
    {
      out.Puts("\tcol1 = dolphin_calculate_lighting_chn1(col1, frag_input.position, frag_input.normal);\n");
    }
    else
    {
      // The number of colors available to TEV is determined by numColorChans.
      // We have to provide the fields to match the interface, so set to zero if it's not enabled.
      out.Puts("\tcol1 = vec4(0.0, 0.0, 0.0, 0.0);\n");
    }
  }

  if (uid_data->genMode_numtexgens == 0)
  {
    // TODO: This is a hack to ensure that shaders still compile when setting out of bounds tex
    // coord indices to 0.  Ideally, it shouldn't exist at all, but the exact behavior hasn't been
    // tested.
    out.Puts("\tint2 fixpoint_uv0 = int2(0, 0);\n"
              "\n");
  }
  else
  {
    out.SetConstantsUsed(C_TEXDIMS, C_TEXDIMS + uid_data->genMode_numtexgens - 1);
    for (u32 i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      out.Printf("\tint2 fixpoint_uv%u = int2(", i);
      out.Printf("(frag_input.tex%u.z == 0.0 ? frag_input.tex%u.xy : frag_input.tex%u.xy / frag_input.tex%u.z)", i, i, i, i);
      out.Printf(" * float2("
                I_TEXDIMS
                "[%u].zw * 128));\n", i);
      // TODO: S24 overflows here?
    }
  }

  for (u32 i = 0; i < uid_data->genMode_numindstages; ++i)
  {
    if ((uid_data->nIndirectStagesUsed & (1U << i)) != 0)
    {
      u32 texcoord = uid_data->GetTevindirefCoord(i);
      const u32 texmap = uid_data->GetTevindirefMap(i);

      // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord
      // does not exist), then tex coord 0 is used (though sometimes glitchy effects happen on
      // console). This affects the Mario portrait in Luigi's Mansion, where the developers forgot
      // to set the number of tex gens to 2 (bug 11462).
      if (texcoord >= uid_data->genMode_numtexgens)
        texcoord = 0;

      out.SetConstantsUsed(C_INDTEXSCALE + i / 2, C_INDTEXSCALE + i / 2);
      out.Printf("\ttempcoord = fixpoint_uv%u >> "
                I_INDTEXSCALE
                "[%u].%s;\n", texcoord, i / 2, (i & 1) ? "zw" : "xy");

      out.Printf("\tint3 iindtex%u = sampleTextureWrapper(%uu, tempcoord, layer).abg;\n", i, texmap);
    }
  }

  for (u32 i = 0; i < numStages; i++)
  {
    // Build the equation for this stage
    WriteStage(out, uid_data, i, api_type, stereo);
  }

  {
    // The results of the last texenv stage are put onto the screen,
    // regardless of the used destination register
    TevStageCombiner::ColorCombiner last_cc;
    TevStageCombiner::AlphaCombiner last_ac;
    last_cc.hex = uid_data->stagehash[uid_data->genMode_numtevstages].cc;
    last_ac.hex = uid_data->stagehash[uid_data->genMode_numtevstages].ac;
    if (last_cc.dest != TevOutput::Prev)
    {
      out.Printf("\tprev.rgb = %s;\n", tev_c_output_table[last_cc.dest]);
    }
    if (last_ac.dest != TevOutput::Prev)
    {
      out.Printf("\tprev.a = %s;\n", tev_a_output_table[last_ac.dest]);
    }
  }

  out.Puts("\tfrag_output.last_texture = rawtextemp;\n");
  out.Puts("\tfrag_output.main = prev;\n");
}

static void WriteFragmentDefinitions(APIType api_type, const ShaderHostConfig& host_config,
                                     const pixel_shader_uid_data* uid_data, ShaderCode& out)
{
  out.Puts("struct DolphinFragmentInput\n");
  out.Puts("{\n");
  out.Puts("\tvec4 color_0;\n");
  out.Puts("\tvec4 color_1;\n");
  out.Puts("\tint layer;\n");
  out.Puts("\tvec3 normal;\n");
  out.Puts("\tvec3 position;\n");
  for (u32 i = 0; i < 8; i++)
  {
    out.Printf("\tvec3 tex%u;\n", i);
  }
  out.Puts("};\n"
            "\n");

  out.Puts("struct DolphinFragmentOutput\n");
  out.Puts("{\n");
  out.Puts("\tivec4 main;\n");
  out.Puts("\tivec4 last_texture;\n");
  out.Puts("};\n"
            "\n");
}

static void WriteEmulatedFragmentBodyHeader(APIType api_type, const ShaderHostConfig& host_config,
                                            const pixel_shader_uid_data* uid_data, ShaderCode& out)
{
  static const char emulated_fragment_definition[] =
      "void dolphin_process_emulated_fragment(in DolphinFragmentInput frag_input, out "
      "DolphinFragmentOutput frag_output)";
  out.Printf("%s\n", emulated_fragment_definition);
  out.Puts("{\n");

  WriteFragmentBody(api_type, host_config, uid_data, out);

  out.Puts("}\n");
}

/* ---- built by calls ---- */

namespace
{
/* e as a statement: if (cond) stmt; */
uint32_t pc_if(const struct shader_build* sb, uint32_t cond, uint32_t then_s, uint32_t else_s)
{
  return rslc_b_if(sb->b, cond, then_s, else_s);
}

uint32_t pc_set_stmt(const struct shader_build* sb, uint32_t l, uint32_t r)
{
  return rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r));
}

void pc_return(const struct shader_build* sb, uint32_t e)
{
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, e));
}

uint32_t pc_call(const struct shader_build* sb, const struct rslc_func* f, unsigned n, ...)
{
  uint32_t a[8];
  unsigned k;
  va_list ap;
  va_start(ap, n);
  for (k = 0; k < n && k < 8; k++)
    a[k] = va_arg(ap, uint32_t);
  va_end(ap);
  return rslc_b_call(sb->b, f, a, n);
}

/* int idot(intN x, intN y) */
struct rslc_func* pc_idot(const struct shader_build* sb, unsigned n)
{
  static const char* const names[2] = {"x", "y"};
  static const unsigned dirs[2] = {RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* types[2];
  struct rslc_func* f;
  struct rslc_var* tmp;
  uint32_t e;
  types[0] = types[1] = sb->i[n];
  f = rslc_b_func(sb->b, "idot", sb->i[1], 2, types, dirs, names);
  rslc_b_func_begin(sb->b, f);
  tmp = rslc_b_local(sb->b, "tmp", sb->i[n],
                     sb_bin(sb, RSLC_B_MUL, sb_ref(sb, rslc_b_param(f, 0)),
                            sb_ref(sb, rslc_b_param(f, 1))),
                     false);
  e = sb_bin(sb, RSLC_B_ADD,
             sb_bin(sb, RSLC_B_ADD, sb_swz(sb, sb_ref(sb, tmp), "x"),
                    sb_swz(sb, sb_ref(sb, tmp), "y")),
             sb_swz(sb, sb_ref(sb, tmp), "z"));
  if (n == 4)
    e = sb_bin(sb, RSLC_B_ADD, e, sb_swz(sb, sb_ref(sb, tmp), "w"));
  pc_return(sb, e);
  rslc_b_func_end(sb->b);
  return f;
}

/* intN iround(floatN x) { return intN(round(x)); } */
struct rslc_func* pc_iround(const struct shader_build* sb, unsigned n)
{
  static const char* const names[1] = {"x"};
  static const unsigned dirs[1] = {RSLC_B_IN};
  const struct rslc_type* types[1];
  struct rslc_func* f;
  types[0] = sb->f[n];
  f = rslc_b_func(sb->b, "iround", sb->i[n], 1, types, dirs, names);
  rslc_b_func_begin(sb->b, f);
  pc_return(sb, sb_con(sb, sb->i[n], 1,
                       sb_call(sb, "round", 1, sb_ref(sb, rslc_b_param(f, 0)))));
  rslc_b_func_end(sb->b);
  return f;
}

u32 pc_uniforms(const struct shader_build* sb, const struct rslc_type** types, const char** names)
{
  static const char* const ps_names[PSU_COUNT] = {
      I_COLORS,          I_KCOLORS,          I_ALPHA,
      I_TEXDIMS,         I_ZBIAS,            I_INDTEXSCALE,
      I_INDTEXMTX,       I_FOGCOLOR,         I_FOGI,
      I_FOGF,            I_FOGRANGE,         I_ZSLOPE,
      I_EFBSCALE,        "bpmem_genmode",    "bpmem_alphaTest",
      "bpmem_fogParam3", "bpmem_fogRangeBase", "bpmem_dstalpha",
      "bpmem_ztex_op",   "bpmem_late_ztest", "bpmem_rgba6_format",
      "bpmem_dither",    "bpmem_bounding_box", "bpmem_pack1",
      "bpmem_pack2",     "konstLookup",      "blend_enable",
      "blend_src_factor", "blend_src_factor_alpha", "blend_dst_factor",
      "blend_dst_factor_alpha", "blend_subtract", "blend_subtract_alpha",
      "logic_op_enable", "logic_op_mode",    "time_ms",
      "xfmem_numTexGen"};
  u32 k;
  for (k = 0; k < PSU_COUNT; k++)
    names[k] = ps_names[k];
  types[PSU_COLORS] = rslc_b_array(sb->b, sb->i[4], 4);
  types[PSU_KCOLORS] = rslc_b_array(sb->b, sb->i[4], 4);
  types[PSU_ALPHA] = sb->i[4];
  types[PSU_TEXDIMS] = rslc_b_array(sb->b, sb->i[4], 8);
  types[PSU_ZBIAS] = rslc_b_array(sb->b, sb->i[4], 2);
  types[PSU_INDTEXSCALE] = rslc_b_array(sb->b, sb->i[4], 2);
  types[PSU_INDTEXMTX] = rslc_b_array(sb->b, sb->i[4], 6);
  types[PSU_FOGCOLOR] = sb->i[4];
  types[PSU_FOGI] = sb->i[4];
  types[PSU_FOGF] = sb->f[4];
  types[PSU_FOGRANGE] = rslc_b_array(sb->b, sb->f[4], 3);
  types[PSU_ZSLOPE] = sb->f[4];
  types[PSU_EFBSCALE] = sb->f[2];
  for (k = PSU_GENMODE; k <= PSU_ZTEX_OP; k++)
    types[k] = sb->u[1];
  for (k = PSU_LATE_ZTEST; k <= PSU_BOUNDING_BOX; k++)
    types[k] = sb->bl;
  types[PSU_PACK1] = rslc_b_array(sb->b, sb->u[4], 16);
  types[PSU_PACK2] = rslc_b_array(sb->b, sb->u[4], 8);
  types[PSU_KONSTLOOKUP] = rslc_b_array(sb->b, sb->i[4], 32);
  types[PSU_BLEND_ENABLE] = sb->bl;
  for (k = PSU_BLEND_SRC_FACTOR; k <= PSU_BLEND_DST_FACTOR_ALPHA; k++)
    types[k] = sb->u[1];
  types[PSU_BLEND_SUBTRACT] = sb->bl;
  types[PSU_BLEND_SUBTRACT_ALPHA] = sb->bl;
  types[PSU_LOGIC_OP_ENABLE] = sb->bl;
  types[PSU_LOGIC_OP_MODE] = sb->u[1];
  types[PSU_TIME_MS] = sb->u[1];
  types[PSU_NUMTEXGEN] = sb->u[1];
  return PSU_COUNT;
}

/* if (bbox_data[k] cmp v) atomicOp(bbox_data[k], v); */
void pc_bbox_update(const struct shader_build* sb, struct rslc_var* bbox_data, int k,
                    enum rslc_b_binop cmp, const char* atomic, struct rslc_var* pos,
                    const char* c)
{
#define BB() sb_idx(sb, sb_ref(sb, bbox_data), sb_i(sb, k))
  rslc_b_stmt(sb->b,
              pc_if(sb, sb_bin(sb, cmp, BB(), sb_swz(sb, sb_ref(sb, pos), c)),
                    rslc_b_expr(sb->b, sb_call(sb, atomic, 2, BB(),
                                               sb_swz(sb, sb_ref(sb, pos), c))),
                    0));
#undef BB
}

void pc_bounding_box(const struct shader_build* sb, struct ps_common* c)
{
  static const char* const buf_names[2] = {"min_pos", "max_pos"};
  static const char* const box_names[1] = {"rawpos"};
  static const unsigned dirs[2] = {RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* types[2];
  const char* names[1];
  struct rslc_b_quals q;
  struct rslc_var *bbox_data, *rawpos, *scale, *pos, *pos_tl, *pos_br;
  struct rslc_func* update;
  uint32_t then_s, e;

  types[0] = rslc_b_array(sb->b, sb->i[1], 4);
  names[0] = "bbox_data";
  rslc_b_quals_init(&q);
  sb_ssbo(sb, &q, 0);
  q.access = RSLC_B_COHERENT;
  rslc_b_block_q(sb->b, &q, "BBox", nullptr, types, names, 1, &bbox_data);

  types[0] = types[1] = sb->i[2];
  update = rslc_b_func(sb->b, "UpdateBoundingBoxBuffer", sb->v, 2, types, dirs, buf_names);
  rslc_b_func_begin(sb->b, update);
  pc_bbox_update(sb, bbox_data, 0, RSLC_B_GT, "atomicMin", rslc_b_param(update, 0), "x");
  pc_bbox_update(sb, bbox_data, 1, RSLC_B_LT, "atomicMax", rslc_b_param(update, 1), "x");
  pc_bbox_update(sb, bbox_data, 2, RSLC_B_GT, "atomicMin", rslc_b_param(update, 0), "y");
  pc_bbox_update(sb, bbox_data, 3, RSLC_B_LT, "atomicMax", rslc_b_param(update, 1), "y");
  rslc_b_func_end(sb->b);

  types[0] = sb->f[2];
  c->update_bounding_box = rslc_b_func(sb->b, "UpdateBoundingBox", sb->v, 1, types, dirs,
                                       box_names);
  rslc_b_func_begin(sb->b, c->update_bounding_box);
  rawpos = rslc_b_param(c->update_bounding_box, 0);
  // We only want to include coordinates for pixels aligned with the native resolution pixel
  // centers. This makes bounding box sizes more accurate (though not perfect) at higher
  // resolutions, avoiding EFB copy buffer overflow in affected games.
  //
  // For a more detailed explanation, see https://dolp.in/pr9801
  scale = rslc_b_local(
      sb->b, "int_efb_scale", sb->i[2],
      pc_call(sb, c->iround[2], 1,
              sb_bin(sb, RSLC_B_DIV, sb_f(sb, 1.0),
                     sb_swz(sb, sb_ref(sb, c->ps[PSU_EFBSCALE]), "xy"))),
      false);
#define PC_OFF_CENTER(axis)                                                                        \
  sb_bin(sb, RSLC_B_NE,                                                                            \
         sb_bin(sb, RSLC_B_MOD,                                                                    \
                sb_con(sb, sb->i[1], 1, sb_swz(sb, sb_ref(sb, rawpos), axis)),                     \
                sb_swz(sb, sb_ref(sb, scale), axis)),                                              \
         sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, scale), axis), sb_i(sb, 1)))
  rslc_b_begin_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_return(sb->b, 0));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, pc_if(sb, sb_bin(sb, RSLC_B_OR, PC_OFF_CENTER("x"), PC_OFF_CENTER("y")),
                           then_s, 0));
#undef PC_OFF_CENTER

  // The rightmost shaded pixel is not included in the right bounding box register,
  // such that width = right - left + 1. This has been verified on hardware.
  pos = rslc_b_local(sb->b, "pos", sb->i[2],
                     sb_con(sb, sb->i[2], 1,
                            sb_bin(sb, RSLC_B_MUL, sb_ref(sb, rawpos),
                                   sb_swz(sb, sb_ref(sb, c->ps[PSU_EFBSCALE]), "xy"))),
                     false);
  // We need to invert the Y coordinate due to OpenGL's lower-left origin
  if (sb->api == SHADER_BUILD_OPENGL)
    sb_set(sb, sb_swz(sb, sb_ref(sb, pos), "y"),
           sb_bin(sb, RSLC_B_SUB,
                  sb_bin(sb, RSLC_B_SUB, sb_i(sb, EFB_HEIGHT), sb_swz(sb, sb_ref(sb, pos), "y")),
                  sb_i(sb, 1)));

  // The GC/Wii GPU rasterizes in 2x2 pixel groups, so bounding box values will be rounded to the
  // extents of these groups, rather than the exact pixel.
  pos_tl = rslc_b_local(sb->b, "pos_tl", sb->i[2],
                        sb_bin(sb, RSLC_B_BAND, sb_ref(sb, pos),
                               sb_un(sb, RSLC_B_BITNOT, sb_i(sb, 1))),
                        false);  // round down to even
  pos_br = rslc_b_local(sb->b, "pos_br", sb->i[2],
                        sb_bin(sb, RSLC_B_BOR, sb_ref(sb, pos), sb_i(sb, 1)),
                        false);  // round up to odd

  if ((sb->flags & SHADER_BUILD_SUBGROUP) && !(sb->flags & SHADER_BUILD_SUBGROUP_BROKEN_DISCARD))
  {
    e = sb_bi(sb, "gl_HelperInvocation");
    if (sb->flags & SHADER_BUILD_INVERTED_HELPER)
      e = sb_un(sb, RSLC_B_NOT, e);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, pos_tl), sb_call(sb, "subgroupMin", 1, sb_ref(sb, pos_tl)));
    sb_set(sb, sb_ref(sb, pos_br), sb_call(sb, "subgroupMax", 1, sb_ref(sb, pos_br)));
    rslc_b_stmt(sb->b, pc_if(sb, sb_call(sb, "subgroupElect", 0),
                             rslc_b_expr(sb->b, pc_call(sb, update, 2, sb_ref(sb, pos_tl),
                                                        sb_ref(sb, pos_br))),
                             0));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, pc_if(sb, sb_un(sb, RSLC_B_NOT, e), then_s, 0));
  }
  else
  {
    sb_do(sb, pc_call(sb, update, 2, sb_ref(sb, pos_tl), sb_ref(sb, pos_br)));
  }
  rslc_b_func_end(sb->b);
}

/* int(BFE) or BFE != 0u locals of a manual sample */
struct pc_sample
{
  const struct shader_build* sb;
  struct ps_common* c;
  struct rslc_var *texmap, *tex, *uv, *layer;
  struct rslc_var *wrap_s, *wrap_t, *size_s, *size_t_;
};

/* WrapCoord(e, wrap, size >> shift) */
uint32_t pc_wrap(const struct pc_sample* s, uint32_t e, struct rslc_var* wrap,
                 struct rslc_var* size, uint32_t shift)
{
  const struct shader_build* sb = s->sb;
  return pc_call(sb, s->c->wrap_coord, 3, e, sb_ref(sb, wrap),
                 sb_bin(sb, RSLC_B_SHR, sb_ref(sb, size), shift));
}

/* base_lod or (base_lod + 1) */
uint32_t pc_lod(const struct shader_build* sb, struct rslc_var* base_lod, bool next)
{
  return next ? sb_bin(sb, RSLC_B_ADD, sb_ref(sb, base_lod), sb_i(sb, 1)) :
                sb_ref(sb, base_lod);
}

/* uint2(WrapCoord(((uv.x >> lod) + d) >> 7, ...), WrapCoord(((uv.y >> lod) + d) >> 7, ...)) */
uint32_t pc_linear_uv(const struct pc_sample* s, struct rslc_var* base_lod, bool next, int d)
{
  const struct shader_build* sb = s->sb;
  uint32_t e[2];
  int k;
  for (k = 0; k < 2; k++)
  {
    uint32_t c = sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, s->uv), k ? "y" : "x"),
                        pc_lod(sb, base_lod, next));
    c = d < 0 ? sb_bin(sb, RSLC_B_SUB, c, sb_i(sb, -d)) : sb_bin(sb, RSLC_B_ADD, c, sb_i(sb, d));
    e[k] = pc_wrap(s, sb_bin(sb, RSLC_B_SHR, c, sb_i(sb, 7)), k ? s->wrap_t : s->wrap_s,
                   k ? s->size_t_ : s->size_s, pc_lod(sb, base_lod, next));
  }
  return sb_con(sb, sb->u[2], 2, e[0], e[1]);
}

/* int2(((uv.x >> lod) - 64) & 0x7f, ((uv.y >> lod) - 64) & 0x7f) */
uint32_t pc_frac_uv(const struct pc_sample* s, struct rslc_var* base_lod, bool next)
{
  const struct shader_build* sb = s->sb;
  uint32_t e[2];
  int k;
  for (k = 0; k < 2; k++)
    e[k] = sb_bin(sb, RSLC_B_BAND,
                  sb_bin(sb, RSLC_B_SUB,
                         sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, s->uv), k ? "y" : "x"),
                                pc_lod(sb, base_lod, next)),
                         sb_i(sb, 64)),
                  sb_i(sb, 0x7f));
  return sb_con(sb, sb->i[2], 2, e[0], e[1]);
}

/* uint2(WrapCoord(uv.x >> (7 + base_lod [+ 1]), ...), ...) */
uint32_t pc_point_uv(const struct pc_sample* s, struct rslc_var* base_lod, bool next)
{
  const struct shader_build* sb = s->sb;
  uint32_t e[2];
  int k;
  for (k = 0; k < 2; k++)
  {
    uint32_t sh = sb_bin(sb, RSLC_B_ADD, sb_i(sb, 7), sb_ref(sb, base_lod));
    if (next)
      sh = sb_bin(sb, RSLC_B_ADD, sh, sb_i(sb, 1));
    e[k] = pc_wrap(s, sb_bin(sb, RSLC_B_SHR, sb_swz(sb, sb_ref(sb, s->uv), k ? "y" : "x"), sh),
                   k ? s->wrap_t : s->wrap_s, k ? s->size_t_ : s->size_s,
                   pc_lod(sb, base_lod, next));
  }
  return sb_con(sb, sb->u[2], 2, e[0], e[1]);
}

/* the manual sampling half of sampleTexture */
void pc_sample_manual(const struct shader_build* sb, const ShaderHostConfig& host_config,
                      const struct sb_bitfield* bf, struct pc_sample* s)
{
  struct ps_common* c = s->c;
  struct rslc_var *texmode0, *texmode1, *mag_linear, *mipmap_linear, *min_linear, *diag_lod,
      *lod_bias, *min_lod, *max_lod, *uv_delta_x, *uv_delta_y, *uv_delta, *max_delta, *lod,
      *is_linear, *base_lod, *frac_lod;
  uint32_t then_s, else_s, inner;

#define TM0(field) BuildBitfieldExtract<&SamplerState::TM0::field>(sb, bf, sb_ref(sb, texmode0))
#define TM1(field) BuildBitfieldExtract<&SamplerState::TM1::field>(sb, bf, sb_ref(sb, texmode1))
#define NZ(e) sb_bin(sb, RSLC_B_NE, e, sb_u(sb, 0u))
#define PACK2(c_) sb_swz(sb, sb_idx(sb, sb_ref(sb, s->c->ps[PSU_PACK2]), sb_ref(sb, s->texmap)), c_)
  texmode0 = rslc_b_local(sb->b, "texmode0", sb->u[1], PACK2("z"), false);
  texmode1 = rslc_b_local(sb->b, "texmode1", sb->u[1], PACK2("w"), false);
  s->wrap_s = rslc_b_local(sb->b, "wrap_s", sb->u[1], TM0(wrap_u), false);
  s->wrap_t = rslc_b_local(sb->b, "wrap_t", sb->u[1], TM0(wrap_v), false);
  mag_linear = rslc_b_local(sb->b, "mag_linear", sb->bl, NZ(TM0(mag_filter)), false);
  mipmap_linear = rslc_b_local(sb->b, "mipmap_linear", sb->bl, NZ(TM0(mipmap_filter)), false);
  min_linear = rslc_b_local(sb->b, "min_linear", sb->bl, NZ(TM0(min_filter)), false);
  diag_lod = rslc_b_local(sb->b, "diag_lod", sb->bl, NZ(TM0(diag_lod)), false);
  lod_bias = rslc_b_local(sb->b, "lod_bias", sb->i[1], TM0(lod_bias), false);
  // uint max_aniso = TODO;
  rslc_b_local(sb->b, "lod_clamp", sb->bl, NZ(TM0(lod_clamp)), false);
  min_lod = rslc_b_local(sb->b, "min_lod", sb->i[1], sb_con(sb, sb->i[1], 1, TM1(min_lod)),
                         false);
  max_lod = rslc_b_local(sb->b, "max_lod", sb->i[1], sb_con(sb, sb->i[1], 1, TM1(max_lod)),
                         false);
#undef PACK2
#undef NZ
#undef TM1
#undef TM0

#define TEXDIM(c_)                                                                                  \
  sb_swz(sb, sb_idx(sb, sb_ref(sb, s->c->ps[PSU_TEXDIMS]), sb_ref(sb, s->texmap)), c_)
  if (host_config.manual_texture_sampling_custom_texture_sizes)
  {
    struct rslc_var *native_s, *native_t, *size, *num_layers, *levels;
    native_s = rslc_b_local(sb->b, "native_size_s", sb->i[1], TEXDIM("x"), false);
    native_t = rslc_b_local(sb->b, "native_size_t", sb->i[1], TEXDIM("y"), false);
    size = rslc_b_local(sb->b, "size", sb->i[3],
                        sb_call(sb, "textureSize", 2, sb_ref(sb, s->tex), sb_i(sb, 0)), false);
    s->size_s = rslc_b_local(sb->b, "size_s", sb->i[1], sb_swz(sb, sb_ref(sb, size), "x"), false);
    s->size_t_ =
        rslc_b_local(sb->b, "size_t", sb->i[1], sb_swz(sb, sb_ref(sb, size), "y"), false);
    num_layers =
        rslc_b_local(sb->b, "num_layers", sb->i[1], sb_swz(sb, sb_ref(sb, size), "z"), false);
    if (g_backend_info.bSupportsTextureQueryLevels)
    {
      levels = rslc_b_local(sb->b, "number_of_levels", sb->i[1],
                            sb_call(sb, "textureQueryLevels", 1, sb_ref(sb, s->tex)), false);
    }
    else
    {
      levels = rslc_b_local(sb->b, "number_of_levels", sb->i[1], sb_i(sb, 256), false);
      ERROR_LOG_FMT(VIDEO, "textureQueryLevels is not supported!  Odd graphical results may "
                           "occur if custom textures are in use!");
    }
    // Prevent out-of-bounds LOD values when using custom textures
    sb_set(sb, sb_ref(sb, max_lod),
           sb_call(sb, "min", 2, sb_ref(sb, max_lod),
                   sb_bin(sb, RSLC_B_SHL,
                          sb_bin(sb, RSLC_B_SUB, sb_ref(sb, levels), sb_i(sb, 1)),
                          sb_i(sb, 4))));
    // Rescale uv to account for the new texture size
    sb_set(sb, sb_swz(sb, sb_ref(sb, s->uv), "x"),
           sb_bin(sb, RSLC_B_DIV,
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, s->uv), "x"),
                         sb_ref(sb, s->size_s)),
                  sb_ref(sb, native_s)));
    sb_set(sb, sb_swz(sb, sb_ref(sb, s->uv), "y"),
           sb_bin(sb, RSLC_B_DIV,
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, s->uv), "y"),
                         sb_ref(sb, s->size_t_)),
                  sb_ref(sb, native_t)));
    // Clamp layer as well (texture() automatically clamps, but texelFetch() doesn't)
    sb_set(sb, sb_ref(sb, s->layer),
           sb_call(sb, "clamp", 3, sb_ref(sb, s->layer), sb_i(sb, 0),
                   sb_bin(sb, RSLC_B_SUB, sb_ref(sb, num_layers), sb_i(sb, 1))));
  }
  else
  {
    s->size_s = rslc_b_local(sb->b, "size_s", sb->i[1], TEXDIM("x"), false);
    s->size_t_ = rslc_b_local(sb->b, "size_t", sb->i[1], TEXDIM("y"), false);
  }
#undef TEXDIM

  {
    // The software renderer uses the equivalent of coarse derivatives, so use them here for
    // consistency.  This hasn't been hardware tested.
    // Note that bSupportsCoarseDerivatives being false only means dFdxCoarse and dFdxFine don't
    // exist.  The GPU may still implement dFdx using coarse derivatives; we just don't have the
    // ability to specifically require it.
    const bool coarse = g_backend_info.bSupportsCoarseDerivatives;
    uv_delta_x = rslc_b_local(
        sb->b, "uv_delta_x", sb->f[2],
        sb_call(sb, "abs", 1,
                sb_call(sb, coarse ? "dFdxCoarse" : "dFdx", 1,
                        sb_con(sb, sb->f[2], 1, sb_ref(sb, s->uv)))),
        false);
    uv_delta_y = rslc_b_local(
        sb->b, "uv_delta_y", sb->f[2],
        sb_call(sb, "abs", 1,
                sb_call(sb, coarse ? "dFdyCoarse" : "dFdy", 1,
                        sb_con(sb, sb->f[2], 1, sb_ref(sb, s->uv)))),
        false);
  }

  // TODO: LOD bias is normally S2.5 (Dolphin uses S7.8 for arbitrary mipmap detection and higher
  // IRs), but (at least per the software renderer) actual LOD is S28.4.  How does this work?
  // Also, note that we can make some assumptions due to use of a SamplerState version of the BP
  // configuration, which tidies things compared to whatever nonsense games can put in.
  uv_delta = rslc_b_local(
      sb->b, "uv_delta", sb->f[2],
      rslc_b_select(sb->b, sb_ref(sb, diag_lod),
                    sb_bin(sb, RSLC_B_ADD, sb_ref(sb, uv_delta_x), sb_ref(sb, uv_delta_y)),
                    sb_call(sb, "max", 2, sb_ref(sb, uv_delta_x), sb_ref(sb, uv_delta_y))),
      false);
  max_delta = rslc_b_local(
      sb->b, "max_delta", sb->f[1],
      sb_call(sb, "max", 2,
              sb_bin(sb, RSLC_B_DIV, sb_swz(sb, sb_ref(sb, uv_delta), "x"), sb_f(sb, 128.0)),
              sb_bin(sb, RSLC_B_DIV, sb_swz(sb, sb_ref(sb, uv_delta), "y"), sb_f(sb, 128.0))),
      false);
  // log2(x) is undefined if x <= 0, but in practice it seems log2(0) is -infinity, which becomes
  // INT_MIN. If lod_bias is negative, adding it to INT_MIN causes an underflow, resulting in a
  // large positive value. Hardware testing indicates that min_lod should be used when the
  // derivative is 0.
  lod = rslc_b_local(
      sb->b, "lod", sb->i[1],
      rslc_b_select(
          sb->b, sb_bin(sb, RSLC_B_EQ, sb_ref(sb, max_delta), sb_f(sb, 0.0)), sb_ref(sb, min_lod),
          sb_bin(sb, RSLC_B_ADD,
                 sb_con(sb, sb->i[1], 1,
                        sb_call(sb, "floor", 1,
                                sb_bin(sb, RSLC_B_MUL,
                                       sb_call(sb, "log2", 1, sb_ref(sb, max_delta)),
                                       sb_f(sb, 16.0)))),
                 sb_bin(sb, RSLC_B_SHR, sb_ref(sb, lod_bias), sb_i(sb, 4)))),
      false);

  is_linear = rslc_b_local(
      sb->b, "is_linear", sb->bl,
      rslc_b_select(sb->b, sb_bin(sb, RSLC_B_GT, sb_ref(sb, lod), sb_i(sb, 0)),
                    sb_ref(sb, min_linear), sb_ref(sb, mag_linear)),
      false);
  sb_set(sb, sb_ref(sb, lod),
         sb_call(sb, "clamp", 3, sb_ref(sb, lod), sb_ref(sb, min_lod), sb_ref(sb, max_lod)));
  base_lod = rslc_b_local(sb->b, "base_lod", sb->i[1],
                          sb_bin(sb, RSLC_B_SHR, sb_ref(sb, lod), sb_i(sb, 4)), false);
  frac_lod = rslc_b_local(sb->b, "frac_lod", sb->i[1],
                          sb_bin(sb, RSLC_B_BAND, sb_ref(sb, lod), sb_i(sb, 15)), false);
  rslc_b_begin_block(sb->b);
  // Round to nearest LOD in point mode
  sb_do(sb, sb_un(sb, RSLC_B_POSTINC, sb_ref(sb, base_lod)));
  then_s = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b,
              pc_if(sb,
                    sb_bin(sb, RSLC_B_AND, sb_un(sb, RSLC_B_NOT, sb_ref(sb, mipmap_linear)),
                           sb_bin(sb, RSLC_B_GE, sb_ref(sb, frac_lod), sb_i(sb, 8))),
                    then_s, 0));

#define PC_NEXT_LOD_COND()                                                                         \
  sb_bin(sb, RSLC_B_AND, sb_bin(sb, RSLC_B_NE, sb_ref(sb, frac_lod), sb_i(sb, 0)),                 \
         sb_ref(sb, mipmap_linear))
  /* linear */
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var *texuv1, *texuv2, *frac_uv, *result;
    texuv1 = rslc_b_local(sb->b, "texuv1", sb->u[2], pc_linear_uv(s, base_lod, false, -64),
                          false);
    texuv2 = rslc_b_local(sb->b, "texuv2", sb->u[2], pc_linear_uv(s, base_lod, false, 64),
                          false);
    frac_uv = rslc_b_local(sb->b, "frac_uv", sb->i[2], pc_frac_uv(s, base_lod, false), false);
    result = rslc_b_local(
        sb->b, "result", sb->i[4],
        pc_call(sb, c->read_texture_linear, 6, sb_ref(sb, s->tex), sb_ref(sb, texuv1),
                sb_ref(sb, texuv2), sb_ref(sb, s->layer), sb_ref(sb, base_lod),
                sb_ref(sb, frac_uv)),
        false);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, texuv1), pc_linear_uv(s, base_lod, true, -64));
    sb_set(sb, sb_ref(sb, texuv2), pc_linear_uv(s, base_lod, true, 64));
    sb_set(sb, sb_ref(sb, frac_uv), pc_frac_uv(s, base_lod, true));
    sb_op_set(sb, RSLC_B_MUL, sb_ref(sb, result),
              sb_bin(sb, RSLC_B_SUB, sb_i(sb, 16), sb_ref(sb, frac_lod)));
    sb_op_set(sb, RSLC_B_ADD, sb_ref(sb, result),
              sb_bin(sb, RSLC_B_MUL,
                     pc_call(sb, c->read_texture_linear, 6, sb_ref(sb, s->tex),
                             sb_ref(sb, texuv1), sb_ref(sb, texuv2), sb_ref(sb, s->layer),
                             pc_lod(sb, base_lod, true), sb_ref(sb, frac_uv)),
                     sb_ref(sb, frac_lod)));
    sb_op_set(sb, RSLC_B_SHR, sb_ref(sb, result), sb_i(sb, 4));
    inner = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, pc_if(sb, PC_NEXT_LOD_COND(), inner, 0));
    pc_return(sb, sb_ref(sb, result));
  }
  then_s = rslc_b_end_block(sb->b);

  /* point */
  rslc_b_begin_block(sb->b);
  {
    struct rslc_var *texuv, *result;
    texuv = rslc_b_local(sb->b, "texuv", sb->u[2], pc_point_uv(s, base_lod, false), false);
    result = rslc_b_local(
        sb->b, "result", sb->i[4],
        pc_call(sb, c->read_texture, 5, sb_ref(sb, s->tex), sb_swz(sb, sb_ref(sb, texuv), "x"),
                sb_swz(sb, sb_ref(sb, texuv), "y"), sb_ref(sb, s->layer), sb_ref(sb, base_lod)),
        false);
    rslc_b_begin_block(sb->b);
    sb_set(sb, sb_ref(sb, texuv), pc_point_uv(s, base_lod, true));
    sb_op_set(sb, RSLC_B_MUL, sb_ref(sb, result),
              sb_bin(sb, RSLC_B_SUB, sb_i(sb, 16), sb_ref(sb, frac_lod)));
    sb_op_set(sb, RSLC_B_ADD, sb_ref(sb, result),
              sb_bin(sb, RSLC_B_MUL,
                     pc_call(sb, c->read_texture, 5, sb_ref(sb, s->tex),
                             sb_swz(sb, sb_ref(sb, texuv), "x"), sb_swz(sb, sb_ref(sb, texuv), "y"),
                             sb_ref(sb, s->layer), pc_lod(sb, base_lod, true)),
                     sb_ref(sb, frac_lod)));
    sb_op_set(sb, RSLC_B_SHR, sb_ref(sb, result), sb_i(sb, 4));
    inner = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, pc_if(sb, PC_NEXT_LOD_COND(), inner, 0));
    pc_return(sb, sb_ref(sb, result));
  }
  else_s = rslc_b_end_block(sb->b);
#undef PC_NEXT_LOD_COND
  rslc_b_stmt(sb->b, pc_if(sb, sb_ref(sb, is_linear), then_s, else_s));
}

/* readTexture, readTextureLinear, SafeModulo and WrapCoord */
void pc_manual_functions(const struct shader_build* sb, const ShaderHostConfig& host_config,
                         struct ps_common* c)
{
  static const char* const rt_names[5] = {"tex", "u", "v", "layer", "lod"};
  static const char* const rtl_names[6] = {"tex", "uv1", "uv2", "layer", "lod", "frac_uv"};
  static const char* const sm_names[2] = {"dividend", "divisor"};
  static const char* const wc_names[3] = {"coord", "wrap", "size"};
  static const unsigned dirs[6] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN,
                                   RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* sampler =
      rslc_b_sampler_type(sb->b, RSLC_B_SAMPLER_2D_ARRAY, RSLC_BASE_FLOAT);
  const struct rslc_type* types[6];
  struct rslc_func* f;
  struct rslc_var *coord, *wrap, *size;
  uint32_t body, then_s, else_s, terms[4];
  int k;

  types[0] = sampler;
  types[1] = types[2] = sb->u[1];
  types[3] = types[4] = sb->i[1];
  f = c->read_texture = rslc_b_func(sb->b, "readTexture", sb->i[4], 5, types, dirs, rt_names);
  rslc_b_func_begin(sb->b, f);
  pc_return(sb, pc_call(sb, c->iround[4], 1,
                        sb_bin(sb, RSLC_B_MUL,
                               sb_call(sb, "texelFetch", 3, sb_ref(sb, rslc_b_param(f, 0)),
                                       sb_con(sb, sb->i[3], 3, sb_ref(sb, rslc_b_param(f, 1)),
                                              sb_ref(sb, rslc_b_param(f, 2)),
                                              sb_ref(sb, rslc_b_param(f, 3))),
                                       sb_ref(sb, rslc_b_param(f, 4))),
                               sb_f(sb, 255.0))));
  rslc_b_func_end(sb->b);

  types[1] = types[2] = sb->u[2];
  types[3] = types[4] = sb->i[1];
  types[5] = sb->i[2];
  f = c->read_texture_linear =
      rslc_b_func(sb->b, "readTextureLinear", sb->i[4], 6, types, dirs, rtl_names);
  rslc_b_func_begin(sb->b, f);
  for (k = 0; k < 4; k++)
  {
    /* readTexture(tex, uvA.x, uvB.y, layer, lod) * (128 - frac_uv.x or frac_uv.x) * (...) */
    struct rslc_var* uv_x = rslc_b_param(f, (k & 1) ? 2 : 1);
    struct rslc_var* uv_y = rslc_b_param(f, (k & 2) ? 2 : 1);
    uint32_t fx = sb_swz(sb, sb_ref(sb, rslc_b_param(f, 5)), "x");
    uint32_t fy = sb_swz(sb, sb_ref(sb, rslc_b_param(f, 5)), "y");
    if (!(k & 1))
      fx = sb_bin(sb, RSLC_B_SUB, sb_i(sb, 128), fx);
    if (!(k & 2))
      fy = sb_bin(sb, RSLC_B_SUB, sb_i(sb, 128), fy);
    terms[k] = sb_bin(sb, RSLC_B_MUL,
                      sb_bin(sb, RSLC_B_MUL,
                             pc_call(sb, c->read_texture, 5, sb_ref(sb, rslc_b_param(f, 0)),
                                     sb_swz(sb, sb_ref(sb, uv_x), "x"),
                                     sb_swz(sb, sb_ref(sb, uv_y), "y"),
                                     sb_ref(sb, rslc_b_param(f, 3)),
                                     sb_ref(sb, rslc_b_param(f, 4))),
                             fx),
                      fy);
  }
  {
    struct rslc_var* result = rslc_b_local(
        sb->b, "result", sb->i[4],
        sb_bin(sb, RSLC_B_ADD,
               sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_ADD, terms[0], terms[1]), terms[2]),
               terms[3]),
        false);
    pc_return(sb, sb_bin(sb, RSLC_B_SHR, sb_ref(sb, result), sb_i(sb, 14)));
  }
  rslc_b_func_end(sb->b);

  if (host_config.manual_texture_sampling_custom_texture_sizes)
  {
    // This is slower, and doesn't result in the same odd behavior that happens on console when
    // wrapping with non-power-of-2 sizes, but it's fine for custom textures to have non-console
    // behavior.
    // Both GLSL and HLSL produce undefined values when the modulo operator (%) is used with a
    // negative dividend and a positive divisor.  We want a positive value such that
    // SafeModulo(-1, 3) is 2.
    struct rslc_var *dividend, *divisor;
    types[0] = types[1] = sb->i[1];
    f = c->safe_modulo = rslc_b_func(sb->b, "SafeModulo", sb->i[1], 2, types, dirs, sm_names);
    rslc_b_func_begin(sb->b, f);
    dividend = rslc_b_param(f, 0);
    divisor = rslc_b_param(f, 1);
    rslc_b_begin_block(sb->b);
    pc_return(sb, sb_bin(sb, RSLC_B_MOD, sb_ref(sb, dividend), sb_ref(sb, divisor)));
    then_s = rslc_b_end_block(sb->b);
    rslc_b_begin_block(sb->b);
    // This works because ~x is the same as -x - 1.
    // `~x % 5` over -5 to -1 gives 4, 3, 2, 1, 0.  `4 - (~x % 5)` gives 0, 1, 2, 3, 4.
    pc_return(sb, sb_bin(sb, RSLC_B_SUB,
                         sb_bin(sb, RSLC_B_SUB, sb_ref(sb, divisor), sb_i(sb, 1)),
                         sb_bin(sb, RSLC_B_MOD, sb_un(sb, RSLC_B_BITNOT, sb_ref(sb, dividend)),
                                sb_ref(sb, divisor))));
    else_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, pc_if(sb, sb_bin(sb, RSLC_B_GE, sb_ref(sb, dividend), sb_i(sb, 0)),
                             then_s, else_s));
    rslc_b_func_end(sb->b);
  }

  types[0] = sb->i[1];
  types[1] = sb->u[1];
  types[2] = sb->i[1];
  f = c->wrap_coord = rslc_b_func(sb->b, "WrapCoord", sb->u[1], 3, types, dirs, wc_names);
  rslc_b_func_begin(sb->b, f);
  coord = rslc_b_param(f, 0);
  wrap = rslc_b_param(f, 1);
  size = rslc_b_param(f, 2);
  rslc_b_begin_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(WrapMode::Clamp))));
  // confirmed that clamp is used for invalid (3) via hardware test
  rslc_b_stmt(sb->b, rslc_b_default(sb->b));
  pc_return(sb, sb_con(sb, sb->u[1], 1,
                       sb_call(sb, "clamp", 3, sb_ref(sb, coord), sb_i(sb, 0),
                               sb_bin(sb, RSLC_B_SUB, sb_ref(sb, size), sb_i(sb, 1)))));
  rslc_b_stmt(sb->b,
              rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(WrapMode::Repeat))));
  if (c->safe_modulo)
  {
    pc_return(sb, sb_con(sb, sb->u[1], 1,
                         pc_call(sb, c->safe_modulo, 2, sb_ref(sb, coord), sb_ref(sb, size))));
  }
  else
  {
    pc_return(sb, sb_con(sb, sb->u[1], 1,
                         sb_bin(sb, RSLC_B_BAND, sb_ref(sb, coord),
                                sb_bin(sb, RSLC_B_SUB, sb_ref(sb, size), sb_i(sb, 1)))));
  }
  rslc_b_stmt(sb->b,
              rslc_b_case(sb->b, static_cast<int32_t>(ShaderEnumValue(WrapMode::Mirror))));
  rslc_b_begin_block(sb->b);
  sb_set(sb, sb_ref(sb, coord), sb_un(sb, RSLC_B_BITNOT, sb_ref(sb, coord)));
  then_s = rslc_b_end_block(sb->b);
  if (c->safe_modulo)
  {
    rslc_b_stmt(sb->b,
                pc_if(sb,
                      sb_bin(sb, RSLC_B_GE,
                             pc_call(sb, c->safe_modulo, 2, sb_ref(sb, coord),
                                     sb_bin(sb, RSLC_B_MUL, sb_i(sb, 2), sb_ref(sb, size))),
                             sb_ref(sb, size)),
                      then_s, 0));
    pc_return(sb, sb_con(sb, sb->u[1], 1,
                         pc_call(sb, c->safe_modulo, 2, sb_ref(sb, coord), sb_ref(sb, size))));
  }
  else
  {
    rslc_b_stmt(sb->b,
                pc_if(sb,
                      sb_bin(sb, RSLC_B_NE,
                             sb_bin(sb, RSLC_B_BAND, sb_ref(sb, coord), sb_ref(sb, size)),
                             sb_i(sb, 0)),
                      then_s, 0));
    pc_return(sb, sb_con(sb, sb->u[1], 1,
                         sb_bin(sb, RSLC_B_BAND, sb_ref(sb, coord),
                                sb_bin(sb, RSLC_B_SUB, sb_ref(sb, size), sb_i(sb, 1)))));
  }
  body = rslc_b_end_block(sb->b);
  rslc_b_stmt(sb->b, rslc_b_switch(sb->b, sb_ref(sb, wrap), body));
  rslc_b_func_end(sb->b);
}
}  // namespace

void BuildPixelShaderCommonHeader(const struct shader_build* sb,
                                  const ShaderHostConfig& host_config, bool bounding_box,
                                  const struct sb_bitfield* bitfield, struct ps_common* c)
{
  static const char* const st_names[4] = {"texmap", "tex", "uv", "layer"};
  static const unsigned dirs[4] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* types[int(SU_COUNT) + int(PSU_COUNT)];
  const char* names[int(SU_COUNT) + int(PSU_COUNT)];
  const struct rslc_type* sampler;
  struct rslc_b_quals q;
  struct pc_sample s;
  unsigned k;

  memset(c, 0, sizeof(*c));
  // dot product for integer vectors
  c->idot3 = pc_idot(sb, 3);
  c->idot4 = pc_idot(sb, 4);
  // rounding + casting to integer at once in a single function
  for (k = 1; k <= 4; k++)
    c->iround[k] = pc_iround(sb, k);

  sampler = rslc_b_sampler_type(sb->b, RSLC_B_SAMPLER_2D_ARRAY, RSLC_BASE_FLOAT);
  rslc_b_quals_init(&q);
  sb_sampler(sb, &q, 0);
  c->samp = rslc_b_declare(sb->b, &q, "samp", rslc_b_array(sb->b, sampler, 8), 0);

  rslc_b_quals_init(&q);
  sb_ubo(sb, &q, 1);
  rslc_b_block_q(sb->b, &q, "PSBlock", nullptr, types, names, pc_uniforms(sb, types, names),
                 c->ps);

  if (host_config.per_pixel_lighting)
  {
    const struct rslc_type* light = BuildLightStruct(sb);
    rslc_b_quals_init(&q);
    sb_ubo(sb, &q, 2);
    rslc_b_block_q(sb->b, &q, "VSBlock", nullptr, types, names,
                   BuildShaderUniforms(sb, light, types, names, 0), c->vs);
  }

  if (bounding_box)
    pc_bounding_box(sb, c);

  if (host_config.manual_texture_sampling)
    pc_manual_functions(sb, host_config, c);

  types[0] = sb->u[1];
  types[1] = sampler;
  types[2] = sb->i[2];
  types[3] = sb->i[1];
  c->sample_texture = rslc_b_func(sb->b, "sampleTexture", sb->i[4], 4, types, dirs, st_names);
  rslc_b_func_begin(sb->b, c->sample_texture);
  memset(&s, 0, sizeof(s));
  s.sb = sb;
  s.c = c;
  s.texmap = rslc_b_param(c->sample_texture, 0);
  s.tex = rslc_b_param(c->sample_texture, 1);
  s.uv = rslc_b_param(c->sample_texture, 2);
  s.layer = rslc_b_param(c->sample_texture, 3);
  if (!host_config.manual_texture_sampling)
  {
    struct rslc_var *size_s, *size_t_, *coords;
    uint32_t e;
#define TEXDIM(c_)                                                                                 \
  sb_swz(sb, sb_idx(sb, sb_ref(sb, c->ps[PSU_TEXDIMS]), sb_ref(sb, s.texmap)), c_)
    size_s = rslc_b_local(
        sb->b, "size_s", sb->f[1],
        sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_MUL, TEXDIM("x"), sb_i(sb, 128))), false);
    size_t_ = rslc_b_local(
        sb->b, "size_t", sb->f[1],
        sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_MUL, TEXDIM("y"), sb_i(sb, 128))), false);
#undef TEXDIM
    coords = rslc_b_local(
        sb->b, "coords", sb->f[3],
        sb_con(sb, sb->f[3], 3,
               sb_bin(sb, RSLC_B_DIV,
                      sb_con(sb, sb->f[1], 1, sb_swz(sb, sb_ref(sb, s.uv), "x")),
                      sb_ref(sb, size_s)),
               sb_bin(sb, RSLC_B_DIV,
                      sb_con(sb, sb->f[1], 1, sb_swz(sb, sb_ref(sb, s.uv), "y")),
                      sb_ref(sb, size_t_)),
               sb_ref(sb, s.layer)),
        false);
    if (!host_config.backend_sampler_lod_bias)
    {
      struct rslc_var *texmode0, *lod_bias;
      texmode0 = rslc_b_local(
          sb->b, "texmode0", sb->u[1],
          sb_swz(sb, sb_idx(sb, sb_ref(sb, c->ps[PSU_PACK2]), sb_ref(sb, s.texmap)), "z"),
          false);
      lod_bias = rslc_b_local(
          sb->b, "lod_bias", sb->f[1],
          sb_bin(sb, RSLC_B_DIV,
                 sb_con(sb, sb->f[1], 1,
                        BuildBitfieldExtract<&SamplerState::TM0::lod_bias>(
                            sb, bitfield, sb_ref(sb, texmode0))),
                 sb_f(sb, 256.0)),
          false);
      e = sb_call(sb, "texture", 3, sb_ref(sb, s.tex), sb_ref(sb, coords), sb_ref(sb, lod_bias));
    }
    else
    {
      e = sb_call(sb, "texture", 2, sb_ref(sb, s.tex), sb_ref(sb, coords));
    }
    pc_return(sb, pc_call(sb, c->iround[4], 1, sb_bin(sb, RSLC_B_MUL, sb_f(sb, 255.0), e)));
  }
  else
  {
    pc_sample_manual(sb, host_config, bitfield, &s);
  }
  rslc_b_func_end(sb->b);
}

namespace
{
/* DolphinFragmentInput's members */
enum
{
  FIN_COLOR_0,
  FIN_COLOR_1,
  FIN_LAYER,
  FIN_NORMAL,
  FIN_POSITION,
  FIN_TEX0
};
enum
{
  FOUT_MAIN,
  FOUT_LAST_TEXTURE
};

struct ps_build
{
  const struct shader_build* sb;
  const pixel_shader_uid_data* uid_data;
  const ShaderHostConfig* host_config;
  APIType api_type;
  struct sb_bitfield bitfield;
  struct ps_common c;
  /* the fragment body's locals */
  struct rslc_var *col0, *col1, *layer, *c0, *c1, *c2, *prev, *rastemp, *rawtextemp, *textemp,
      *konsttemp, *comp16, *comp24, *alphabump, *tevcoord, *wrappedcoord, *tempcoord, *tevin[4];
  struct rslc_var *fixpoint_uv[8], *iindtex[4];
};

uint32_t pb_r(const struct ps_build* p, struct rslc_var* v)
{
  return sb_ref(p->sb, v);
}

uint32_t pb_swz(const struct ps_build* p, struct rslc_var* v, const char* s)
{
  return sb_swz(p->sb, sb_ref(p->sb, v), s);
}

/* intN(v, v, ...) of an int literal, negative ones as the text writes them */
uint32_t pb_ivec(const struct shader_build* sb, unsigned n, int v)
{
  uint32_t a[4];
  unsigned k;
  for (k = 0; k < n; k++)
    a[k] = v < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -v)) : sb_i(sb, v);
  if (n == 1)
    return a[0];
  return rslc_b_construct(sb->b, sb->i[n], a, n);
}

/* a literal int as the text writes it */
uint32_t pb_int(const struct shader_build* sb, int v)
{
  return v < 0 ? sb_un(sb, RSLC_B_NEG, sb_i(sb, -v)) : sb_i(sb, v);
}

/* sampleTextureWrapper(texmap, uv, layer) */
uint32_t pb_sample(const struct ps_build* p, u32 texmap, uint32_t uv)
{
  const struct shader_build* sb = p->sb;
  uint32_t a[4];
  a[0] = sb_u(sb, texmap);
  a[1] = sb_idx(sb, pb_r(p, p->c.samp), sb_u(sb, texmap));
  a[2] = uv;
  a[3] = pb_r(p, p->layer);
  return rslc_b_call(sb->b, p->c.sample_texture, a, 4);
}

/* tev_c_input_table */
uint32_t pb_c_input(const struct ps_build* p, TevColorArg arg)
{
  const struct shader_build* sb = p->sb;
  switch (arg)
  {
  case TevColorArg::PrevColor:
    return pb_swz(p, p->prev, "rgb");
  case TevColorArg::PrevAlpha:
    return pb_swz(p, p->prev, "aaa");
  case TevColorArg::Color0:
    return pb_swz(p, p->c0, "rgb");
  case TevColorArg::Alpha0:
    return pb_swz(p, p->c0, "aaa");
  case TevColorArg::Color1:
    return pb_swz(p, p->c1, "rgb");
  case TevColorArg::Alpha1:
    return pb_swz(p, p->c1, "aaa");
  case TevColorArg::Color2:
    return pb_swz(p, p->c2, "rgb");
  case TevColorArg::Alpha2:
    return pb_swz(p, p->c2, "aaa");
  case TevColorArg::TexColor:
    return pb_swz(p, p->textemp, "rgb");
  case TevColorArg::TexAlpha:
    return pb_swz(p, p->textemp, "aaa");
  case TevColorArg::RasColor:
    return pb_swz(p, p->rastemp, "rgb");
  case TevColorArg::RasAlpha:
    return pb_swz(p, p->rastemp, "aaa");
  case TevColorArg::One:
    return pb_ivec(sb, 3, 255);
  case TevColorArg::Half:
    return pb_ivec(sb, 3, 128);
  case TevColorArg::Konst:
    return pb_swz(p, p->konsttemp, "rgb");
  case TevColorArg::Zero:
  default:
    return pb_ivec(sb, 3, 0);
  }
}

/* tev_a_input_table */
uint32_t pb_a_input(const struct ps_build* p, TevAlphaArg arg)
{
  switch (arg)
  {
  case TevAlphaArg::PrevAlpha:
    return pb_swz(p, p->prev, "a");
  case TevAlphaArg::Alpha0:
    return pb_swz(p, p->c0, "a");
  case TevAlphaArg::Alpha1:
    return pb_swz(p, p->c1, "a");
  case TevAlphaArg::Alpha2:
    return pb_swz(p, p->c2, "a");
  case TevAlphaArg::TexAlpha:
    return pb_swz(p, p->textemp, "a");
  case TevAlphaArg::RasAlpha:
    return pb_swz(p, p->rastemp, "a");
  case TevAlphaArg::Konst:
    return pb_swz(p, p->konsttemp, "a");
  case TevAlphaArg::Zero:
  default:
    return sb_i(p->sb, 0);
  }
}

/* tev_c_output_table / tev_a_output_table */
struct rslc_var* pb_output(const struct ps_build* p, TevOutput dest)
{
  switch (dest)
  {
  case TevOutput::Color0:
    return p->c0;
  case TevOutput::Color1:
    return p->c1;
  case TevOutput::Color2:
    return p->c2;
  case TevOutput::Prev:
  default:
    return p->prev;
  }
}

/* WriteTevRegular: (d + bias + lerp(a,b,c)) * scale over components */
uint32_t pb_tev_regular(const struct ps_build* p, const char* s, TevBias bias, TevOp op,
                        TevScale scale)
{
  const struct shader_build* sb = p->sb;
  uint32_t d, a, l;
  // Regular TEV stage: (d + bias + lerp(a,b,c)) * scale
  // The GameCube/Wii GPU uses a very sophisticated algorithm for scale-lerping:
  // - c is scaled from 0..255 to 0..256, which allows dividing the result by 256 instead of 255
  // - if scale is bigger than one, it is moved inside the lerp calculation for increased accuracy
  // - a rounding bias is added before dividing by 256
  // TODO: Is the rounding bias still added when the scale is divide by 2?  Currently we do not
  // apply it.
#define TI(k) pb_swz(p, p->tevin[k], s)
  d = TI(3);
  if (bias == TevBias::AddHalf)
    d = sb_bin(sb, RSLC_B_ADD, d, sb_i(sb, 128));
  else if (bias == TevBias::SubHalf)
    d = sb_bin(sb, RSLC_B_SUB, d, sb_i(sb, 128));
  if (scale == TevScale::Scale2)
    d = sb_bin(sb, RSLC_B_SHL, d, sb_i(sb, 1));
  else if (scale == TevScale::Scale4)
    d = sb_bin(sb, RSLC_B_SHL, d, sb_i(sb, 2));

  a = sb_bin(sb, RSLC_B_ADD, sb_bin(sb, RSLC_B_SHL, TI(0), sb_i(sb, 8)),
             sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_SUB, TI(1), TI(0)),
                    sb_bin(sb, RSLC_B_ADD, TI(2), sb_bin(sb, RSLC_B_SHR, TI(2), sb_i(sb, 7)))));
#undef TI
  if (scale == TevScale::Scale2)
    a = sb_bin(sb, RSLC_B_SHL, a, sb_i(sb, 1));
  else if (scale == TevScale::Scale4)
    a = sb_bin(sb, RSLC_B_SHL, a, sb_i(sb, 2));
  if (scale != TevScale::Divide2)
    a = sb_bin(sb, RSLC_B_ADD, a, sb_i(sb, op == TevOp::Sub ? 127 : 128));
  l = sb_bin(sb, RSLC_B_SHR, a, sb_i(sb, 8));
  d = sb_bin(sb, op == TevOp::Sub ? RSLC_B_SUB : RSLC_B_ADD, d, l);
  if (scale == TevScale::Divide2)
    d = sb_bin(sb, RSLC_B_SHR, d, sb_i(sb, 1));
  return d;
}

/* idot(tevin_x.rgb, compN) */
uint32_t pb_comp(const struct ps_build* p, int k, struct rslc_var* comp)
{
  const struct shader_build* sb = p->sb;
  uint32_t a[2];
  a[0] = pb_swz(p, p->tevin[k], "rgb");
  a[1] = sb_ref(sb, comp);
  return rslc_b_call(sb->b, p->c.idot3, a, 2);
}

/* the compare of a stage: (cmp ? tevin_c.s : zero) */
uint32_t pb_compare(const struct ps_build* p, TevCompareMode mode, bool eq, bool alpha)
{
  const struct shader_build* sb = p->sb;
  const enum rslc_b_binop op = eq ? RSLC_B_EQ : RSLC_B_GT;
  const char* s = alpha ? "a" : "rgb";
  uint32_t cond;
  switch (mode)
  {
  case TevCompareMode::R8:
    cond = sb_bin(sb, op, pb_swz(p, p->tevin[0], "r"), pb_swz(p, p->tevin[1], "r"));
    break;
  case TevCompareMode::GR16:
    cond = sb_bin(sb, op, pb_comp(p, 0, p->comp16), pb_comp(p, 1, p->comp16));
    break;
  case TevCompareMode::BGR24:
    cond = sb_bin(sb, op, pb_comp(p, 0, p->comp24), pb_comp(p, 1, p->comp24));
    break;
  case TevCompareMode::RGB8:
  default:
    if (alpha)
    {
      cond = sb_bin(sb, op, pb_swz(p, p->tevin[0], "a"), pb_swz(p, p->tevin[1], "a"));
      break;
    }
    if (eq)
      return sb_bin(sb, RSLC_B_MUL,
                    sb_bin(sb, RSLC_B_SUB, pb_ivec(sb, 3, 1),
                           sb_call(sb, "sign", 1,
                                   sb_call(sb, "abs", 1,
                                           sb_bin(sb, RSLC_B_SUB,
                                                  pb_swz(p, p->tevin[0], "rgb"),
                                                  pb_swz(p, p->tevin[1], "rgb"))))),
                    pb_swz(p, p->tevin[2], "rgb"));
    return sb_bin(sb, RSLC_B_MUL,
                  sb_call(sb, "max", 2,
                          sb_call(sb, "sign", 1,
                                  sb_bin(sb, RSLC_B_SUB, pb_swz(p, p->tevin[0], "rgb"),
                                         pb_swz(p, p->tevin[1], "rgb"))),
                          pb_ivec(sb, 3, 0)),
                  pb_swz(p, p->tevin[2], "rgb"));
  }
  return rslc_b_select(sb->b, cond, pb_swz(p, p->tevin[2], s),
                       alpha ? sb_i(sb, 0) : pb_ivec(sb, 3, 0));
}

/* one of the konst selections, as int4(color, alpha)'s arguments */
unsigned pb_konst(const struct ps_build* p, KonstSel kc, KonstSel ka, uint32_t* args)
{
  static const int ratio[8] = {255, 223, 191, 159, 128, 96, 64, 32};
  static const char* const rgb_swz[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
  static const char* const a_swz[4] = {"r", "g", "b", "a"};
  const struct shader_build* sb = p->sb;
  const u32 c = static_cast<u32>(kc), a = static_cast<u32>(ka);
  unsigned n = 0;
  if (c < 0xC)
  {
    const int v = c < 8 ? ratio[c] : 0;
    args[n++] = sb_i(sb, v);
    args[n++] = sb_i(sb, v);
    args[n++] = sb_i(sb, v);
  }
  else
  {
    args[n++] = sb_swz(sb, sb_idx(sb, pb_r(p, p->c.ps[PSU_KCOLORS]), sb_i(sb, (c - 0xC) % 4)),
                       rgb_swz[(c - 0xC) / 4]);
  }
  if (a < 0x10)
    args[n++] = sb_i(sb, a < 8 ? ratio[a] : 0);
  else
    args[n++] = sb_swz(sb, sb_idx(sb, pb_r(p, p->c.ps[PSU_KCOLORS]), sb_i(sb, (a - 0x10) % 4)),
                       a_swz[(a - 0x10) / 4]);
  return n;
}

/* if (cindmtx[m].w >= 0) t >>= cindmtx[m].w; else t <<= (-cindmtx[m].w); */
void pb_ind_scale(const struct ps_build* p, struct rslc_var* trans, u32 mtxidx, int n)
{
  const struct shader_build* sb = p->sb;
  uint32_t else_s;
#define MW() sb_swz(sb, sb_idx(sb, pb_r(p, p->c.ps[PSU_INDTEXMTX]), sb_i(sb, static_cast<int>(mtxidx))), "w")
  // TODO: should use a shader uid branch for this for better performance
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
  {
    static const char* const names[16] = {
        "indtexmtx_w_inverse_0",  "indtexmtx_w_inverse_1",  "indtexmtx_w_inverse_2",
        "indtexmtx_w_inverse_3",  "indtexmtx_w_inverse_4",  "indtexmtx_w_inverse_5",
        "indtexmtx_w_inverse_6",  "indtexmtx_w_inverse_7",  "indtexmtx_w_inverse_8",
        "indtexmtx_w_inverse_9",  "indtexmtx_w_inverse_10", "indtexmtx_w_inverse_11",
        "indtexmtx_w_inverse_12", "indtexmtx_w_inverse_13", "indtexmtx_w_inverse_14",
        "indtexmtx_w_inverse_15"};
    struct rslc_var* inverse =
        rslc_b_local(sb->b, names[n], sb->i[1], sb_un(sb, RSLC_B_NEG, MW()), false);
    else_s = rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SHL, pb_r(p, trans),
                                              pb_r(p, inverse)));
  }
  else
  {
    else_s = rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SHL, pb_r(p, trans),
                                              sb_un(sb, RSLC_B_NEG, MW())));
  }
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_GE, MW(), sb_i(sb, 0)),
                        rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SHR, pb_r(p, trans),
                                                         MW())),
                        else_s));
#undef MW
}

/* WriteStage */
bool pb_stage(struct ps_build* p, int n)
{
  static const char* const names_iindtevcrd[16] = {
      "iindtevcrd0",  "iindtevcrd1",  "iindtevcrd2",  "iindtevcrd3",  "iindtevcrd4",
      "iindtevcrd5",  "iindtevcrd6",  "iindtevcrd7",  "iindtevcrd8",  "iindtevcrd9",
      "iindtevcrd10", "iindtevcrd11", "iindtevcrd12", "iindtevcrd13", "iindtevcrd14",
      "iindtevcrd15"};
  static const char* const names_indtevtrans[16] = {
      "indtevtrans0",  "indtevtrans1",  "indtevtrans2",  "indtevtrans3",  "indtevtrans4",
      "indtevtrans5",  "indtevtrans6",  "indtevtrans7",  "indtevtrans8",  "indtevtrans9",
      "indtevtrans10", "indtevtrans11", "indtevtrans12", "indtevtrans13", "indtevtrans14",
      "indtevtrans15"};
  static const char rgba[4] = {'r', 'g', 'b', 'a'};
  const struct shader_build* sb = p->sb;
  const pixel_shader_uid_data* uid_data = p->uid_data;
  const auto& stage = uid_data->stagehash[n];
  struct rslc_var* indtevtrans;
  struct rslc_var* fixpoint;

  // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord does not
  // exist), then tex coord 0 is used (though sometimes glitchy effects happen on console).
  u32 texcoord = stage.tevorders_texcoord;
  const bool has_tex_coord = texcoord < uid_data->genMode_numtexgens;
  if (!has_tex_coord)
    texcoord = 0;
  fixpoint = p->fixpoint_uv[texcoord];

  {
    const TevStageIndirect tevind{.hex = stage.tevind};
    // Quirk: Referencing a stage above the number of ind stages is undefined behavior,
    // and on console produces a noise pattern (details unknown).
    // Instead, just skip applying the indirect operation, which is close enough.
    // We need to do *something*, as there won't be an iindtex variable otherwise.
    // Viewtiful Joe hits this case (bug 12525).
    // Wrapping and add to previous still apply in this case (and when the stage is disabled).
    const bool has_ind_stage = tevind.bt < uid_data->genMode_numindstages;
    struct rslc_var* iindtex = has_ind_stage ? p->iindtex[tevind.bt] : nullptr;

    // Perform the indirect op on the incoming regular coordinates
    // using iindtex{} as the offset coords
    if (has_ind_stage && tevind.bs != IndTexBumpAlpha::Off)
    {
      static const char* const sel[4] = {"", "x", "y", "z"};
      // According to libogc, the bump alpha value is 5 bits, and comes from the bottom bits of
      // the component byte, except in the case of ITF_8, which presumably uses the top bits with
      // a mask.
      // https://github.com/devkitPro/libogc/blob/bd24a9b3f59502f9b30d6bac0ae35fc485045f78/gc/ogc/gx.h#L3038-L3041
      // https://github.com/devkitPro/libogc/blob/bd24a9b3f59502f9b30d6bac0ae35fc485045f78/gc/ogc/gx.h#L790-L800
      static const int shift[4] = {0, 5, 4, 3};
      if (!iindtex)
        return false;
      sb_set(sb, pb_r(p, p->alphabump),
             sb_bin(sb, RSLC_B_BAND,
                    sb_bin(sb, RSLC_B_SHL, pb_swz(p, iindtex, sel[u32(tevind.bs.Value())]),
                           sb_i(sb, shift[u32(tevind.fmt.Value())])),
                    sb_i(sb, 248)));
    }

    if (has_ind_stage && tevind.matrix_index != IndMtxIndex::Off)
    {
      // format
      static const int fmt_shift[4] = {0, 3, 4, 5};
      // bias - TODO: Check if this needs to be this complicated...
      static const char* const bias_field[8] = {"", "x", "y", "xy", "z", "xz", "yz", "xyz"};
      static const unsigned bias_n[8] = {0, 1, 1, 2, 1, 2, 2, 3};
      struct rslc_var* crd;
      if (!iindtex)
        return false;
      crd = rslc_b_local(sb->b, names_iindtevcrd[n], sb->i[3],
                         sb_bin(sb, RSLC_B_SHR, pb_r(p, iindtex),
                                sb_i(sb, fmt_shift[u32(tevind.fmt.Value())])),
                         false);
      if (tevind.bias != IndTexBias::None)
      {
        const unsigned bn = bias_n[u32(tevind.bias.Value())];
        const int add = tevind.fmt == IndTexFormat::ITF_8 ? -128 : 1;
        uint32_t e;
        if (bn == 1)
          e = sb_con(sb, sb->i[1], 1, pb_int(sb, add));
        else
          e = pb_ivec(sb, bn, add);
        sb_op_set(sb, RSLC_B_ADD, pb_swz(p, crd, bias_field[u32(tevind.bias.Value())]), e);
      }

      // Multiplied by 2 because each matrix has two rows.
      // Note also that the 4th column of the matrix contains the scale factor.
      const u32 mtxidx = 2 * (static_cast<u32>(tevind.matrix_index.Value()) - 1);

      // multiply by offset matrix and scale - calculations are likely to overflow badly,
      // yet it works out since we only care about the lower 23 bits (+1 sign bit) of the result
      if (tevind.matrix_id == IndMtxId::Indirect)
      {
        uint32_t a[2], b[2];
        a[0] = sb_swz(sb, sb_idx(sb, pb_r(p, p->c.ps[PSU_INDTEXMTX]), sb_i(sb, static_cast<int>(mtxidx))), "xyz");
        a[1] = pb_r(p, crd);
        b[0] = sb_swz(sb, sb_idx(sb, pb_r(p, p->c.ps[PSU_INDTEXMTX]), sb_i(sb, static_cast<int>(mtxidx + 1))),
                      "xyz");
        b[1] = pb_r(p, crd);
        indtevtrans = rslc_b_local(
            sb->b, names_indtevtrans[n], sb->i[2],
            sb_bin(sb, RSLC_B_SHR,
                   sb_con(sb, sb->i[2], 2, rslc_b_call(sb->b, p->c.idot3, a, 2),
                          rslc_b_call(sb->b, p->c.idot3, b, 2)),
                   sb_i(sb, 3)),
            false);
        pb_ind_scale(p, indtevtrans, mtxidx, n);
      }
      else if (tevind.matrix_id == IndMtxId::S || tevind.matrix_id == IndMtxId::T)
      {
        ASSERT(has_tex_coord);
        indtevtrans = rslc_b_local(
            sb->b, names_indtevtrans[n], sb->i[2],
            sb_bin(sb, RSLC_B_SHR,
                   sb_con(sb, sb->i[2], 1,
                          sb_bin(sb, RSLC_B_MUL, pb_r(p, fixpoint),
                                 pb_swz(p, crd, tevind.matrix_id == IndMtxId::S ? "xx" : "yy"))),
                   sb_i(sb, 8)),
            false);
        pb_ind_scale(p, indtevtrans, mtxidx, n);
      }
      else
      {
        indtevtrans = rslc_b_local(sb->b, names_indtevtrans[n], sb->i[2], pb_ivec(sb, 2, 0),
                                   false);
        ASSERT(false);  // Unknown value for matrix_id
      }
    }
    else
    {
      indtevtrans =
          rslc_b_local(sb->b, names_indtevtrans[n], sb->i[2], pb_ivec(sb, 2, 0), false);
      if (tevind.matrix_index == IndMtxIndex::Off)
      {
        // If matrix_index is Off (0), matrix_id should be Indirect (0)
        ASSERT(tevind.matrix_id == IndMtxId::Indirect);
      }
    }

    // ---------
    // Wrapping
    // ---------
    {
      static const int wrap_start[5] = {256, 128, 64, 32, 16};
      const IndTexWrap wraps[2] = {tevind.sw, tevind.tw};
      int k;
      for (k = 0; k < 2; k++)
      {
        const char* c = k ? "y" : "x";
        uint32_t e;
        if (wraps[k] == IndTexWrap::ITW_OFF)
          e = pb_swz(p, fixpoint, c);
        else if (wraps[k] >= IndTexWrap::ITW_0)  // 7 (Invalid) appears to behave as 6 (ITW_0)
          e = sb_i(sb, 0);
        else
          e = sb_bin(sb, RSLC_B_BAND, pb_swz(p, fixpoint, c),
                     sb_bin(sb, RSLC_B_SUB,
                            sb_bin(sb, RSLC_B_SHL,
                                   sb_i(sb, wrap_start[u32(wraps[k]) - u32(IndTexWrap::ITW_256)]),
                                   sb_i(sb, 7)),
                            sb_i(sb, 1)));
        sb_set(sb, pb_swz(p, p->wrappedcoord, c), e);
      }
    }

    sb_op_set(sb, tevind.fb_addprev ? RSLC_B_ADD : RSLC_B_SET, pb_swz(p, p->tevcoord, "xy"),
              sb_bin(sb, RSLC_B_ADD, pb_r(p, p->wrappedcoord), pb_r(p, indtevtrans)));

    // Emulate s24 overflows
    sb_set(sb, pb_swz(p, p->tevcoord, "xy"),
           sb_bin(sb, RSLC_B_SHR,
                  sb_bin(sb, RSLC_B_SHL, pb_swz(p, p->tevcoord, "xy"), sb_i(sb, 8)),
                  sb_i(sb, 8)));
  }

  TevStageCombiner::ColorCombiner cc;
  TevStageCombiner::AlphaCombiner ac;
  cc.hex = stage.cc;
  ac.hex = stage.ac;

  if (cc.a == TevColorArg::RasAlpha || cc.a == TevColorArg::RasColor ||
      cc.b == TevColorArg::RasAlpha || cc.b == TevColorArg::RasColor ||
      cc.c == TevColorArg::RasAlpha || cc.c == TevColorArg::RasColor ||
      cc.d == TevColorArg::RasAlpha || cc.d == TevColorArg::RasColor ||
      ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
      ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha)
  {
    // Generate swizzle string to represent the Ras color channel swapping
    char swz[5];
    uint32_t e;
    swz[0] = rgba[u32(stage.ras_swap_r)];
    swz[1] = rgba[u32(stage.ras_swap_g)];
    swz[2] = rgba[u32(stage.ras_swap_b)];
    swz[3] = rgba[u32(stage.ras_swap_a)];
    swz[4] = '\0';
    switch (stage.tevorders_colorchan)
    {
    case RasColorChan::Color0:
    case RasColorChan::Color1:
    {
      uint32_t x = sb_bin(sb, RSLC_B_MUL,
                          pb_r(p, stage.tevorders_colorchan == RasColorChan::Color0 ? p->col0 :
                                                                                      p->col1),
                          sb_f(sb, 255.0));
      e = rslc_b_call(sb->b, p->c.iround[4], &x, 1);
      break;
    }
    case RasColorChan::AlphaBump:
      e = sb_bin(sb, RSLC_B_MUL, pb_ivec(sb, 4, 1), pb_r(p, p->alphabump));
      break;
    case RasColorChan::NormalizedAlphaBump:
      e = sb_bin(sb, RSLC_B_MUL, pb_ivec(sb, 4, 1),
                 sb_bin(sb, RSLC_B_BOR, pb_r(p, p->alphabump),
                        sb_bin(sb, RSLC_B_SHR, pb_r(p, p->alphabump), sb_i(sb, 5))));
      break;
    case RasColorChan::Zero:
      e = pb_ivec(sb, 4, 0);
      break;
    default:
      /* the text names no such channel */
      return false;
    }
    sb_set(sb, pb_r(p, p->rastemp), sb_swz(sb, e, swz));
  }

  if (stage.tevorders_enable && uid_data->genMode_numtexgens > 0)
  {
    // Generate swizzle string to represent the texture color channel swapping
    char swz[5];
    swz[0] = rgba[u32(stage.tex_swap_r)];
    swz[1] = rgba[u32(stage.tex_swap_g)];
    swz[2] = rgba[u32(stage.tex_swap_b)];
    swz[3] = rgba[u32(stage.tex_swap_a)];
    swz[4] = '\0';
    sb_set(sb, pb_r(p, p->rawtextemp),
           pb_sample(p, stage.tevorders_texmap, pb_swz(p, p->tevcoord, "xy")));
    sb_set(sb, pb_r(p, p->textemp), pb_swz(p, p->rawtextemp, swz));
  }
  else if (uid_data->genMode_numtexgens == 0)
  {
    // It seems like the result is always black when no tex coords are enabled, but further
    // testing is needed.
    sb_set(sb, pb_r(p, p->textemp), pb_ivec(sb, 4, 0));
  }
  else
  {
    sb_set(sb, pb_r(p, p->textemp), pb_ivec(sb, 4, 255));
  }

  if (cc.a == TevColorArg::Konst || cc.b == TevColorArg::Konst || cc.c == TevColorArg::Konst ||
      cc.d == TevColorArg::Konst || ac.a == TevAlphaArg::Konst || ac.b == TevAlphaArg::Konst ||
      ac.c == TevAlphaArg::Konst || ac.d == TevAlphaArg::Konst)
  {
    uint32_t args[4];
    const unsigned na = pb_konst(p, stage.tevksel_kc, stage.tevksel_ka, args);
    sb_set(sb, pb_r(p, p->konsttemp), rslc_b_construct(sb->b, sb->i[4], args, na));
  }

  {
    const TevColorArg ca[3] = {cc.a, cc.b, cc.c};
    const TevAlphaArg aa[3] = {ac.a, ac.b, ac.c};
    int k;
    for (k = 0; k < 3; k++)
    {
      if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VECTOR_BITWISE_AND))
        sb_set(sb, pb_r(p, p->tevin[k]),
               sb_con(sb, sb->i[4], 2, sb_bin(sb, RSLC_B_BAND, pb_c_input(p, ca[k]), sb_i(sb, 255)),
                      sb_bin(sb, RSLC_B_BAND, pb_a_input(p, aa[k]), sb_i(sb, 255))));
      else
        sb_set(sb, pb_r(p, p->tevin[k]),
               sb_bin(sb, RSLC_B_BAND, sb_con(sb, sb->i[4], 2, pb_c_input(p, ca[k]),
                                              pb_a_input(p, aa[k])),
                      pb_ivec(sb, 4, 255)));
    }
    sb_set(sb, pb_r(p, p->tevin[3]),
           sb_con(sb, sb->i[4], 2, pb_c_input(p, cc.d), pb_a_input(p, ac.d)));
  }

  // color combine
  {
    uint32_t e;
    if (cc.bias != TevBias::Compare)
      e = pb_tev_regular(p, "rgb", cc.bias, cc.op, cc.scale);
    else
      e = sb_bin(sb, RSLC_B_ADD, pb_swz(p, p->tevin[3], "rgb"),
                 pb_compare(p, cc.compare_mode, cc.comparison == TevComparison::EQ, false));
    sb_set(sb, pb_swz(p, pb_output(p, cc.dest), "rgb"),
           sb_call(sb, "clamp", 3, e, pb_ivec(sb, 3, cc.clamp ? 0 : -1024),
                   pb_ivec(sb, 3, cc.clamp ? 255 : 1023)));
  }

  // alpha combine
  {
    uint32_t e;
    if (ac.bias != TevBias::Compare)
      e = pb_tev_regular(p, "a", ac.bias, ac.op, ac.scale);
    else
      e = sb_bin(sb, RSLC_B_ADD, pb_swz(p, p->tevin[3], "a"),
                 pb_compare(p, ac.compare_mode, ac.comparison == TevComparison::EQ, true));
    sb_set(sb, pb_swz(p, pb_output(p, ac.dest), "a"),
           sb_call(sb, "clamp", 3, e, pb_int(sb, ac.clamp ? 0 : -1024),
                   pb_int(sb, ac.clamp ? 255 : 1023)));
  }
  return true;
}

/* WriteFragmentBody: the body of dolphin_process_emulated_fragment */
bool pb_fragment_body(struct ps_build* p, struct rslc_var* fin, struct rslc_var* fout,
                      struct rslc_func* const* lighting)
{
  static const char* const fixpoint_names[8] = {
      "fixpoint_uv0", "fixpoint_uv1", "fixpoint_uv2", "fixpoint_uv3",
      "fixpoint_uv4", "fixpoint_uv5", "fixpoint_uv6", "fixpoint_uv7"};
  static const char* const iindtex_names[4] = {"iindtex0", "iindtex1", "iindtex2", "iindtex3"};
  const struct shader_build* sb = p->sb;
  const pixel_shader_uid_data* uid_data = p->uid_data;
  const u32 num_stages = uid_data->genMode_numtevstages + 1;
  u32 i;

#define FIN(k) sb_mem(sb, pb_r(p, fin), static_cast<uint32_t>(k))
#define COLORS(k) sb_idx(sb, pb_r(p, p->c.ps[PSU_COLORS]), sb_i(sb, k))
  p->col0 = rslc_b_local(sb->b, "col0", sb->f[4], FIN(FIN_COLOR_0), false);
  p->col1 = rslc_b_local(sb->b, "col1", sb->f[4], FIN(FIN_COLOR_1), false);
  p->layer = rslc_b_local(sb->b, "layer", sb->i[1], FIN(FIN_LAYER), false);
  p->c0 = rslc_b_local(sb->b, "c0", sb->i[4], COLORS(1), false);
  p->c1 = rslc_b_local(sb->b, "c1", sb->i[4], COLORS(2), false);
  p->c2 = rslc_b_local(sb->b, "c2", sb->i[4], COLORS(3), false);
  p->prev = rslc_b_local(sb->b, "prev", sb->i[4], COLORS(0), false);
#undef COLORS
  p->rastemp = rslc_b_local(sb->b, "rastemp", sb->i[4], pb_ivec(sb, 4, 0), false);
  p->rawtextemp = rslc_b_local(sb->b, "rawtextemp", sb->i[4], pb_ivec(sb, 4, 0), false);
  p->textemp = rslc_b_local(sb->b, "textemp", sb->i[4], pb_ivec(sb, 4, 0), false);
  p->konsttemp = rslc_b_local(sb->b, "konsttemp", sb->i[4], pb_ivec(sb, 4, 0), false);
  p->comp16 = rslc_b_local(sb->b, "comp16", sb->i[3],
                           sb_con(sb, sb->i[3], 3, sb_i(sb, 1), sb_i(sb, 256), sb_i(sb, 0)),
                           false);
  p->comp24 = rslc_b_local(
      sb->b, "comp24", sb->i[3],
      sb_con(sb, sb->i[3], 3, sb_i(sb, 1), sb_i(sb, 256),
             sb_bin(sb, RSLC_B_MUL, sb_i(sb, 256), sb_i(sb, 256))),
      false);
  p->alphabump = rslc_b_local(sb->b, "alphabump", sb->i[1], sb_i(sb, 0), false);
  p->tevcoord = rslc_b_local(sb->b, "tevcoord", sb->i[3], pb_ivec(sb, 3, 0), false);
  p->wrappedcoord = rslc_b_local(sb->b, "wrappedcoord", sb->i[2], pb_ivec(sb, 2, 0), false);
  p->tempcoord = rslc_b_local(sb->b, "tempcoord", sb->i[2], pb_ivec(sb, 2, 0), false);
  {
    static const char* const names[4] = {"tevin_a", "tevin_b", "tevin_c", "tevin_d"};
    int k;
    for (k = 0; k < 4; k++)
      p->tevin[k] = rslc_b_local(sb->b, names[k], sb->i[4], pb_ivec(sb, 4, 0), false);
  }

  if (p->host_config->per_pixel_lighting)
  {
    uint32_t a[3];
    if (uid_data->numColorChans > 0)
    {
      a[0] = pb_r(p, p->col0);
      a[1] = FIN(FIN_POSITION);
      a[2] = FIN(FIN_NORMAL);
      sb_set(sb, pb_r(p, p->col0), rslc_b_call(sb->b, lighting[0], a, 3));
    }
    else
    {
      // The number of colors available to TEV is determined by numColorChans.
      // We have to provide the fields to match the interface, so set to zero if it's not
      // enabled.
      sb_set(sb, pb_r(p, p->col0),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));
    }
    if (uid_data->numColorChans == 2)
    {
      a[0] = pb_r(p, p->col1);
      a[1] = FIN(FIN_POSITION);
      a[2] = FIN(FIN_NORMAL);
      sb_set(sb, pb_r(p, p->col1), rslc_b_call(sb->b, lighting[1], a, 3));
    }
    else
    {
      sb_set(sb, pb_r(p, p->col1),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0)));
    }
  }

  if (uid_data->genMode_numtexgens == 0)
  {
    // TODO: This is a hack to ensure that shaders still compile when setting out of bounds tex
    // coord indices to 0.  Ideally, it shouldn't exist at all, but the exact behavior hasn't been
    // tested.
    p->fixpoint_uv[0] =
        rslc_b_local(sb->b, "fixpoint_uv0", sb->i[2], pb_ivec(sb, 2, 0), false);
  }
  else
  {
    for (i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
#define TEX(c_) sb_swz(sb, FIN(FIN_TEX0 + static_cast<int>(i)), c_)
      uint32_t uv = rslc_b_select(
          sb->b, sb_bin(sb, RSLC_B_EQ, TEX("z"), sb_f(sb, 0.0)), TEX("xy"),
          sb_bin(sb, RSLC_B_DIV, TEX("xy"), TEX("z")));
#undef TEX
      p->fixpoint_uv[i] = rslc_b_local(
          sb->b, fixpoint_names[i], sb->i[2],
          sb_con(sb, sb->i[2], 1,
                 sb_bin(sb, RSLC_B_MUL, uv,
                        sb_con(sb, sb->f[2], 1,
                               sb_bin(sb, RSLC_B_MUL,
                                      sb_swz(sb,
                                             sb_idx(sb, pb_r(p, p->c.ps[PSU_TEXDIMS]),
                                                    sb_i(sb, static_cast<int>(i))),
                                             "zw"),
                                      sb_i(sb, 128))))),
          false);
      // TODO: S24 overflows here?
    }
  }

  for (i = 0; i < uid_data->genMode_numindstages; ++i)
  {
    if ((uid_data->nIndirectStagesUsed & (1U << i)) != 0)
    {
      u32 texcoord = uid_data->GetTevindirefCoord(i);
      const u32 texmap = uid_data->GetTevindirefMap(i);
      // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord
      // does not exist), then tex coord 0 is used (though sometimes glitchy effects happen on
      // console). This affects the Mario portrait in Luigi's Mansion, where the developers forgot
      // to set the number of tex gens to 2 (bug 11462).
      if (texcoord >= uid_data->genMode_numtexgens)
        texcoord = 0;
      sb_set(sb, pb_r(p, p->tempcoord),
             sb_bin(sb, RSLC_B_SHR, pb_r(p, p->fixpoint_uv[texcoord]),
                    sb_swz(sb,
                           sb_idx(sb, pb_r(p, p->c.ps[PSU_INDTEXSCALE]), sb_i(sb, static_cast<int>(i / 2))),
                           (i & 1) ? "zw" : "xy")));
      p->iindtex[i] = rslc_b_local(
          sb->b, iindtex_names[i], sb->i[3],
          sb_swz(sb, pb_sample(p, texmap, pb_r(p, p->tempcoord)), "abg"), false);
    }
  }

  for (i = 0; i < num_stages; i++)
  {
    // Build the equation for this stage
    if (!pb_stage(p, static_cast<int>(i)))
      return false;
  }

  {
    // The results of the last texenv stage are put onto the screen,
    // regardless of the used destination register
    TevStageCombiner::ColorCombiner last_cc;
    TevStageCombiner::AlphaCombiner last_ac;
    last_cc.hex = uid_data->stagehash[uid_data->genMode_numtevstages].cc;
    last_ac.hex = uid_data->stagehash[uid_data->genMode_numtevstages].ac;
    if (last_cc.dest != TevOutput::Prev)
      sb_set(sb, pb_swz(p, p->prev, "rgb"), pb_swz(p, pb_output(p, last_cc.dest), "rgb"));
    if (last_ac.dest != TevOutput::Prev)
      sb_set(sb, pb_swz(p, p->prev, "a"), pb_swz(p, pb_output(p, last_ac.dest), "a"));
  }

  sb_set(sb, sb_mem(sb, pb_r(p, fout), FOUT_LAST_TEXTURE), pb_r(p, p->rawtextemp));
  sb_set(sb, sb_mem(sb, pb_r(p, fout), FOUT_MAIN), pb_r(p, p->prev));
#undef FIN
  return true;
}

/* (prev.a cmp alphaRef.r or .g), (false) or (true) */
uint32_t pb_alpha_func(const struct ps_build* p, CompareMode mode, const char* ref)
{
  const struct shader_build* sb = p->sb;
  enum rslc_b_binop op;
  switch (mode)
  {
  case CompareMode::Never:
    return rslc_b_bool(sb->b, false);
  case CompareMode::Less:
    op = RSLC_B_LT;
    break;
  case CompareMode::Equal:
    op = RSLC_B_EQ;
    break;
  case CompareMode::LEqual:
    op = RSLC_B_LE;
    break;
  case CompareMode::Greater:
    op = RSLC_B_GT;
    break;
  case CompareMode::NEqual:
    op = RSLC_B_NE;
    break;
  case CompareMode::GEqual:
    op = RSLC_B_GE;
    break;
  case CompareMode::Always:
  default:
    return rslc_b_bool(sb->b, true);
  }
  return sb_bin(sb, op, pb_swz(p, p->prev, "a"), pb_swz(p, p->c.ps[PSU_ALPHA], ref));
}
}  // namespace

namespace
{
/* the stage's inputs: a member of the unnamed block, or a varying */
struct ps_inputs
{
  const struct shader_build* sb;
  struct rslc_var* block;
  struct rslc_var* var[20];
};

uint32_t pi_ref(const struct ps_inputs* in, int idx)
{
  if (in->block)
    return sb_mem(in->sb, sb_ref(in->sb, in->block), static_cast<uint32_t>(idx));
  return sb_ref(in->sb, in->var[idx]);
}

/* intN(v...) as the text writes float literals: float4(0.0, 0.0, 0.0, 0.0) */
uint32_t pb_fvec(const struct shader_build* sb, unsigned n, double v)
{
  uint32_t a[4];
  unsigned k;
  for (k = 0; k < n; k++)
    a[k] = sb_f(sb, v);
  return rslc_b_construct(sb->b, sb->f[n], a, n);
}

/* WriteFog */
void pb_fog(const struct ps_build* p, struct rslc_var* rawpos, struct rslc_var* zcoord)
{
  const struct shader_build* sb = p->sb;
  const pixel_shader_uid_data* uid_data = p->uid_data;
  struct rslc_var *ze, *fog, *ifog;
  uint32_t e;
  if (uid_data->fog_fsel == FogType::Off)
    return;  // no Fog

#define FOGF(c_) sb_swz(sb, pb_r(p, p->c.ps[PSU_FOGF]), c_)
#define FOGI(c_) sb_swz(sb, pb_r(p, p->c.ps[PSU_FOGI]), c_)
  if (uid_data->fog_proj == FogProjection::Perspective)
  {
    // perspective
    // ze = A/(B - (Zs >> B_SHF)
    // TODO: Verify that we want to drop lower bits here! (currently taken over from software
    // renderer)
    //       Maybe we want to use "ze = (A << B_SHF)/((B << B_SHF) - Zs)" instead?
    //       That's equivalent, but keeps the lower bits of Zs.
    e = sb_bin(sb, RSLC_B_DIV, sb_bin(sb, RSLC_B_MUL, FOGF("x"), sb_f(sb, 16777216.0)),
               sb_con(sb, sb->f[1], 1,
                      sb_bin(sb, RSLC_B_SUB, FOGI("y"),
                             sb_bin(sb, RSLC_B_SHR, pb_r(p, zcoord), FOGI("w")))));
  }
  else
  {
    // orthographic
    // ze = a*Zs    (here, no B_SHF)
    e = sb_bin(sb, RSLC_B_DIV,
               sb_bin(sb, RSLC_B_MUL, FOGF("x"), sb_con(sb, sb->f[1], 1, pb_r(p, zcoord))),
               sb_f(sb, 16777216.0));
  }
  ze = rslc_b_local(sb->b, "ze", sb->f[1], e, false);

  // x_adjust = sqrt((x-center)^2 + k^2)/k
  // ze *= x_adjust
  if (uid_data->fog_RangeBaseEnabled)
  {
    struct rslc_var *offset, *floatindex, *lower, *upper, *klower, *kupper, *k, *x_adjust;
    offset = rslc_b_local(
        sb->b, "offset", sb->f[1],
        sb_bin(sb, RSLC_B_SUB,
               sb_bin(sb, RSLC_B_SUB,
                      sb_bin(sb, RSLC_B_MUL, sb_f(sb, 2.0),
                             sb_bin(sb, RSLC_B_DIV, pb_swz(p, rawpos, "x"), FOGF("w"))),
                      sb_f(sb, 1.0)),
               FOGF("z")),
        false);
    floatindex = rslc_b_local(
        sb->b, "floatindex", sb->f[1],
        sb_call(sb, "clamp", 3,
                sb_bin(sb, RSLC_B_SUB, sb_f(sb, 9.0),
                       sb_bin(sb, RSLC_B_MUL, sb_call(sb, "abs", 1, pb_r(p, offset)),
                              sb_f(sb, 9.0))),
                sb_f(sb, 0.0), sb_f(sb, 9.0)),
        false);
    lower = rslc_b_local(sb->b, "indexlower", sb->u[1],
                         sb_con(sb, sb->u[1], 1, pb_r(p, floatindex)), false);
    upper = rslc_b_local(sb->b, "indexupper", sb->u[1],
                         sb_bin(sb, RSLC_B_ADD, pb_r(p, lower), sb_u(sb, 1u)), false);
#define FOGRANGE(v)                                                                                \
  sb_idx(sb,                                                                                       \
         sb_idx(sb, pb_r(p, p->c.ps[PSU_FOGRANGE]),                                                \
                sb_bin(sb, RSLC_B_SHR, pb_r(p, v), sb_u(sb, 2u))),                                 \
         sb_bin(sb, RSLC_B_BAND, pb_r(p, v), sb_u(sb, 3u)))
    klower = rslc_b_local(sb->b, "klower", sb->f[1], FOGRANGE(lower), false);
    kupper = rslc_b_local(sb->b, "kupper", sb->f[1], FOGRANGE(upper), false);
#undef FOGRANGE
    k = rslc_b_local(sb->b, "k", sb->f[1],
                     sb_call(sb, "mix", 3, pb_r(p, klower), pb_r(p, kupper),
                             sb_call(sb, "fract", 1, pb_r(p, floatindex))),
                     false);
    x_adjust = rslc_b_local(
        sb->b, "x_adjust", sb->f[1],
        sb_bin(sb, RSLC_B_DIV,
               sb_call(sb, "sqrt", 1,
                       sb_bin(sb, RSLC_B_ADD,
                              sb_bin(sb, RSLC_B_MUL, pb_r(p, offset), pb_r(p, offset)),
                              sb_bin(sb, RSLC_B_MUL, pb_r(p, k), pb_r(p, k)))),
               pb_r(p, k)),
        false);
    sb_op_set(sb, RSLC_B_MUL, pb_r(p, ze), pb_r(p, x_adjust));
  }

  fog = rslc_b_local(
      sb->b, "fog", sb->f[1],
      sb_call(sb, "clamp", 3, sb_bin(sb, RSLC_B_SUB, pb_r(p, ze), FOGF("y")), sb_f(sb, 0.0),
              sb_f(sb, 1.0)),
      false);
#undef FOGI
#undef FOGF

#define NEG8() sb_un(sb, RSLC_B_NEG, sb_f(sb, 8.0))
  switch (uid_data->fog_fsel)
  {
  case FogType::Exp:
    sb_set(sb, pb_r(p, fog),
           sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0),
                  sb_call(sb, "exp2", 1, sb_bin(sb, RSLC_B_MUL, NEG8(), pb_r(p, fog)))));
    break;
  case FogType::ExpSq:
    sb_set(sb, pb_r(p, fog),
           sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0),
                  sb_call(sb, "exp2", 1,
                          sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_MUL, NEG8(), pb_r(p, fog)),
                                 pb_r(p, fog)))));
    break;
  case FogType::BackwardsExp:
    sb_set(sb, pb_r(p, fog),
           sb_call(sb, "exp2", 1,
                   sb_bin(sb, RSLC_B_MUL, NEG8(),
                          sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), pb_r(p, fog)))));
    break;
  case FogType::BackwardsExpSq:
    sb_set(sb, pb_r(p, fog), sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), pb_r(p, fog)));
    sb_set(sb, pb_r(p, fog),
           sb_call(sb, "exp2", 1,
                   sb_bin(sb, RSLC_B_MUL, sb_bin(sb, RSLC_B_MUL, NEG8(), pb_r(p, fog)),
                          pb_r(p, fog))));
    break;
  default:
    if (uid_data->fog_fsel != FogType::Linear)
      WARN_LOG_FMT(VIDEO, "Unknown Fog Type! {}", uid_data->fog_fsel);
    break;
  }
#undef NEG8

  {
    uint32_t x = sb_bin(sb, RSLC_B_MUL, pb_r(p, fog), sb_f(sb, 256.0));
    ifog = rslc_b_local(sb->b, "ifog", sb->i[1], rslc_b_call(sb->b, p->c.iround[1], &x, 1),
                        false);
  }
  sb_set(sb, pb_swz(p, p->prev, "rgb"),
         sb_bin(sb, RSLC_B_SHR,
                sb_bin(sb, RSLC_B_ADD,
                       sb_bin(sb, RSLC_B_MUL, pb_swz(p, p->prev, "rgb"),
                              sb_bin(sb, RSLC_B_SUB, sb_i(sb, 256), pb_r(p, ifog))),
                       sb_bin(sb, RSLC_B_MUL, pb_swz(p, p->c.ps[PSU_FOGCOLOR], "rgb"),
                              pb_r(p, ifog))),
                sb_i(sb, 8)));
}

/* WriteLogicOp's modes over prev and fb_value */
uint32_t pb_logic_op(const struct ps_build* p, u32 mode, struct rslc_var* fb)
{
  const struct shader_build* sb = p->sb;
#define PREV() pb_r(p, p->prev)
#define FB() pb_r(p, fb)
#define NOT(e) sb_un(sb, RSLC_B_BITNOT, e)
  switch (mode)
  {
  case 0:  // CLEAR
    return pb_ivec(sb, 4, 0);
  case 1:  // AND
    return sb_bin(sb, RSLC_B_BAND, PREV(), FB());
  case 2:  // AND_REVERSE
    return sb_bin(sb, RSLC_B_BAND, PREV(), NOT(FB()));
  case 3:  // COPY
    return PREV();
  case 4:  // AND_INVERTED
    return sb_bin(sb, RSLC_B_BAND, NOT(PREV()), FB());
  case 5:  // NOOP
    return FB();
  case 6:  // XOR
    return sb_bin(sb, RSLC_B_BXOR, PREV(), FB());
  case 7:  // OR
    return sb_bin(sb, RSLC_B_BOR, PREV(), FB());
  case 8:  // NOR
    return NOT(sb_bin(sb, RSLC_B_BOR, PREV(), FB()));
  case 9:  // EQUIV
    return NOT(sb_bin(sb, RSLC_B_BXOR, PREV(), FB()));
  case 10:  // INVERT
    return NOT(FB());
  case 11:  // OR_REVERSE
    return sb_bin(sb, RSLC_B_BOR, PREV(), NOT(FB()));
  case 12:  // COPY_INVERTED
    return NOT(PREV());
  case 13:  // OR_INVERTED
    return sb_bin(sb, RSLC_B_BOR, NOT(PREV()), FB());
  case 14:  // NAND
    return NOT(sb_bin(sb, RSLC_B_BAND, PREV(), FB()));
  case 15:  // SET
  default:
    return pb_ivec(sb, 4, 255);
  }
#undef NOT
#undef FB
#undef PREV
}

/* WriteBlend's factors: (rgb, alpha) of a src or dst factor */
uint32_t pb_blend_factor(const struct ps_build* p, u32 factor, bool dst, bool alpha,
                         struct rslc_var* initial, struct rslc_var* ocol0,
                         struct rslc_var* src_color)
{
  const struct shader_build* sb = p->sb;
  const char* s3 = alpha ? "a" : "rgb";
  const char* aaa = alpha ? "a" : "aaa";
  switch (factor)
  {
  case 0:  // ZERO
    return alpha ? sb_f(sb, 0.0) : sb_con(sb, sb->f[3], 3, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0));
  case 1:  // ONE
    return alpha ? sb_f(sb, 1.0) : sb_con(sb, sb->f[3], 3, sb_i(sb, 1), sb_i(sb, 1), sb_i(sb, 1));
  case 2:  // DSTCLR / SRCCLR
    return pb_swz(p, dst ? ocol0 : initial, s3);
  case 3:  // INVDSTCLR / INVSRCCLR
    return sb_bin(sb, RSLC_B_SUB,
                  alpha ? sb_f(sb, 1.0) :
                          sb_con(sb, sb->f[3], 3, sb_i(sb, 1), sb_i(sb, 1), sb_i(sb, 1)),
                  pb_swz(p, dst ? ocol0 : initial, s3));
  case 4:  // SRCALPHA
    return pb_swz(p, src_color, aaa);
  case 5:  // INVSRCALPHA
    return sb_bin(sb, RSLC_B_SUB,
                  alpha ? sb_f(sb, 1.0) :
                          sb_con(sb, sb->f[3], 3, sb_i(sb, 1), sb_i(sb, 1), sb_i(sb, 1)),
                  pb_swz(p, src_color, aaa));
  case 6:  // DSTALPHA
    return pb_swz(p, initial, aaa);
  case 7:  // INVDSTALPHA
  default:
    return sb_bin(sb, RSLC_B_SUB,
                  alpha ? sb_f(sb, 1.0) :
                          sb_con(sb, sb->f[3], 3, sb_i(sb, 1), sb_i(sb, 1), sb_i(sb, 1)),
                  pb_swz(p, initial, aaa));
  }
}
}  // namespace

bool BuildPixelShader(struct shader_build* sb, const void* arg)
{
  const PixelShaderBuildArgs* a = static_cast<const PixelShaderBuildArgs*>(arg);
  const ShaderHostConfig& host_config = *a->host_config;
  const pixel_shader_uid_data* uid_data = a->uid_data;
  const bool per_pixel_lighting = host_config.per_pixel_lighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  static const char* const tex_names[8] = {"tex0", "tex1", "tex2", "tex3",
                                           "tex4", "tex5", "tex6", "tex7"};
  struct ps_build p;
  struct ps_inputs in;
  struct vs_output_layout plain;
  struct rslc_b_quals q;
  struct rslc_func* lighting[NUM_XF_COLOR_CHANNELS] = {nullptr, nullptr};
  const struct rslc_type *fin_t, *fout_t, *out_t;
  struct rslc_func *emulated, *process, *fmain;
  struct rslc_var *ocol0 = nullptr, *ocol1 = nullptr, *real_ocol0 = nullptr, *in_ocol0 = nullptr;
  struct rslc_var *layer_in = nullptr, *rawpos, *initial = nullptr, *fin, *fout, *zcoord;
  uint32_t args[2];
  u32 i;

  memset(&p, 0, sizeof(p));
  memset(&in, 0, sizeof(in));
  p.sb = sb;
  p.uid_data = uid_data;
  p.host_config = &host_config;
  p.api_type = a->api_type;
  in.sb = sb;

  // Stuff that is shared between ubershaders and pixelgen.
  BuildBitfieldExtractHeader(sb, host_config, &p.bitfield);
  BuildPixelShaderCommonHeader(sb, host_config, uid_data->bounding_box, &p.bitfield, &p.c);

  if (uid_data->ztest == EmulatedZ::ForcedEarly)
  {
    // Zcomploc (aka early_ztest) is a way to control whether depth test is done before or after
    // texturing and alpha test. early_fragment_tests forces the driver to do z and stencil tests
    // early, with discard causing the shader to exit with only the depth buffer updated; OpenGL
    // contexts without it say so with conservative depth's depth_unchanged instead.
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.early_fragment_tests = true;
    rslc_b_layout(sb->b, &q);
  }

  const bool use_framebuffer_fetch = uid_data->blend_enable || uid_data->logic_op_enable ||
                                     uid_data->ztest == EmulatedZ::EarlyWithFBFetch;
  out_t = uid_data->uint_output ? sb->u[4] : sb->f[4];
  rslc_b_quals_init(&q);
  q.storage = RSLC_B_ST_OUT;
  q.location = sb_output_location(sb, 0);
#ifdef __APPLE__
  // Framebuffer fetch is only supported by Metal, so ensure that we're running Vulkan (MoltenVK)
  // if we want to use it.
  if (a->api_type == APIType::Vulkan || a->api_type == APIType::Metal)
  {
    out_t = sb->f[4];
    if (!uid_data->no_dual_src)
    {
      q.index = 0;
      if (use_framebuffer_fetch)
        real_ocol0 = rslc_b_declare(sb->b, &q, "real_ocol0", out_t, 0);
      else
        ocol0 = rslc_b_declare(sb->b, &q, "ocol0", out_t, 0);
      q.index = 1;
      ocol1 = rslc_b_declare(sb->b, &q, "ocol1", out_t, 0);
    }
    else if (use_framebuffer_fetch)
    {
      real_ocol0 = rslc_b_declare(sb->b, &q, "real_ocol0", out_t, 0);
    }
    else
    {
      ocol0 = rslc_b_declare(sb->b, &q, "ocol0", out_t, 0);
    }
  }
  else
#endif
  {
    if (use_framebuffer_fetch)
    {
      real_ocol0 = rslc_b_declare(sb->b, &q, "real_ocol0", sb->f[4], 0);
    }
    else
    {
      q.index = 0;
      ocol0 = rslc_b_declare(sb->b, &q, "ocol0", out_t, 0);
    }
    if (!uid_data->no_dual_src)
    {
      q.index = 1;
      ocol1 = rslc_b_declare(sb->b, &q, "ocol1", out_t, 0);
    }
  }
  if (use_framebuffer_fetch)
  {
    // The colour attachment's value on entry: Metal's framebuffer fetch, or an OpenGL context's
    // framebuffer fetch extension
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_UNIFORM;
    q.input_attachment = 0;
    if (sb->api != SHADER_BUILD_OPENGL)
    {
      q.set = 0;
      q.binding = 0;
    }
    in_ocol0 = rslc_b_declare(sb->b, &q, "in_ocol0",
                              rslc_b_sampler_type(sb->b, RSLC_B_SUBPASS_INPUT, RSLC_BASE_FLOAT),
                              0);
  }

  BuildVSOutputLayout(sb, uid_data->genMode_numtexgens, host_config, 0, &plain);
  if (host_config.backend_geometry_shaders)
  {
    struct vs_output_layout in_l;
    BuildVSOutputLayout(sb, uid_data->genMode_numtexgens, host_config,
                        BuildInterpolationQuals(msaa, ssaa), &in_l);
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.location = sb_varying_location(sb, 0);
    in.block = rslc_b_io_block(sb->b, &q, "VertexData", nullptr, in_l.types, in_l.names,
                               nullptr, in_l.quals, in_l.n, -1);
    if (stereo && !host_config.backend_gl_layer_in_fs)
    {
      rslc_b_quals_init(&q);
      q.storage = RSLC_B_ST_IN;
      q.interp = RSLC_B_INTERP_FLAT;
      layer_in = rslc_b_declare(sb->b, &q, "layer", sb->i[1], 0);
    }
  }
  else
  {
    // Let's set up attributes
    u32 counter = 0;
    rslc_b_quals_init(&q);
    q.storage = RSLC_B_ST_IN;
    q.quals = BuildInterpolationQuals(msaa, ssaa);
#define PS_VARYING(idx, name, type)                                                                \
  do                                                                                               \
  {                                                                                                \
    q.location = sb_varying_location(sb, counter++);                                              \
    in.var[idx] = rslc_b_declare(sb->b, &q, name, type, 0);                                       \
  } while (0)
    PS_VARYING(plain.colors_0, "colors_0", sb->f[4]);
    PS_VARYING(plain.colors_1, "colors_1", sb->f[4]);
    for (i = 0; i < uid_data->genMode_numtexgens; ++i)
      PS_VARYING(plain.tex0 + static_cast<int>(i), tex_names[i], sb->f[3]);
    if (!host_config.fast_depth_calc)
      PS_VARYING(plain.clipPos, "clipPos", sb->f[4]);
    if (per_pixel_lighting)
    {
      PS_VARYING(plain.Normal, "Normal", sb->f[3]);
      PS_VARYING(plain.WorldPos, "WorldPos", sb->f[3]);
    }
#undef PS_VARYING
  }

  if (per_pixel_lighting)
    BuildLightingShaderHeader(sb, uid_data->lighting, p.c.vs[SU_MATERIALS], p.c.vs[SU_LIGHTS],
                              lighting);

  {
    static const char* const fin_names[13] = {"color_0", "color_1", "layer", "normal", "position",
                                              "tex0",    "tex1",    "tex2",  "tex3",   "tex4",
                                              "tex5",    "tex6",    "tex7"};
    static const char* const fout_names[2] = {"main", "last_texture"};
    static const char* const param_names[2] = {"frag_input", "frag_output"};
    static const unsigned dirs[2] = {RSLC_B_IN, RSLC_B_OUT};
    const struct rslc_type* types[13];
    types[0] = types[1] = sb->f[4];
    types[2] = sb->i[1];
    for (i = 3; i < 13; i++)
      types[i] = sb->f[3];
    fin_t = rslc_b_struct(sb->b, "DolphinFragmentInput", types, fin_names, 13);
    types[0] = types[1] = sb->i[4];
    fout_t = rslc_b_struct(sb->b, "DolphinFragmentOutput", types, fout_names, 2);

    types[0] = fin_t;
    types[1] = fout_t;
    emulated = rslc_b_func(sb->b, "dolphin_process_emulated_fragment", sb->v, 2, types, dirs,
                           param_names);
    rslc_b_func_begin(sb->b, emulated);
    if (!pb_fragment_body(&p, rslc_b_param(emulated, 0), rslc_b_param(emulated, 1), lighting))
      return false;
    rslc_b_func_end(sb->b);

    process = rslc_b_func(sb->b, "process_fragment", sb->v, 2, types, dirs, param_names);
    rslc_b_func_begin(sb->b, process);
    args[0] = sb_ref(sb, rslc_b_param(process, 0));
    args[1] = sb_ref(sb, rslc_b_param(process, 1));
    sb_do(sb, rslc_b_call(sb->b, emulated, args, 2));
    rslc_b_func_end(sb->b);
  }

  fmain = rslc_b_func(sb->b, "main", sb->v, 0, nullptr, nullptr, nullptr);
  rslc_b_func_begin(sb->b, fmain);
  rawpos = rslc_b_local(sb->b, "rawpos", sb->f[4], sb_bi(sb, "gl_FragCoord"), false);

  if (use_framebuffer_fetch)
  {
    // Store off a copy of the initial framebuffer value.
    initial = rslc_b_local(sb->b, "initial_ocol0", sb->f[4],
                           sb_call(sb, "subpassLoad", 1, sb_ref(sb, in_ocol0)), false);
    // QComm's Adreno driver doesn't seem to like using the framebuffer_fetch value as an
    // intermediate value with multiple reads & modifications, so we pull out the "real" output
    // value above and use a temporary for calculations, then set the output value once at the
    // end of the shader.
    ocol0 = rslc_b_local(sb->b, "ocol0", sb->f[4], 0, false);
  }
  if (uid_data->blend_enable)
    ocol1 = rslc_b_local(sb->b, "ocol1", sb->f[4], 0, false);

  if (host_config.backend_geometry_shaders && stereo)
  {
    if (host_config.backend_gl_layer_in_fs)
      layer_in = rslc_b_local(sb->b, "layer", sb->i[1], sb_bi(sb, "gl_Layer"), false);
  }
  else
  {
    layer_in = rslc_b_local(sb->b, "layer", sb->i[1], sb_i(sb, 0), false);
  }

  fin = rslc_b_local(sb->b, "frag_input", fin_t, 0, false);
#define FI(k) sb_mem(sb, sb_ref(sb, fin), static_cast<uint32_t>(k))
  sb_set(sb, FI(FIN_COLOR_0), pi_ref(&in, plain.colors_0));
  sb_set(sb, FI(FIN_COLOR_1), pi_ref(&in, plain.colors_1));
  sb_set(sb, FI(FIN_LAYER), sb_ref(sb, layer_in));
  if (per_pixel_lighting)
  {
    sb_set(sb, FI(FIN_NORMAL), sb_call(sb, "normalize", 1, pi_ref(&in, plain.Normal)));
    sb_set(sb, FI(FIN_POSITION), pi_ref(&in, plain.WorldPos));
  }
  else
  {
    sb_set(sb, FI(FIN_NORMAL), sb_con(sb, sb->f[3], 3, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
    sb_set(sb, FI(FIN_POSITION), sb_con(sb, sb->f[3], 3, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
  }
  for (i = 0; i < uid_data->genMode_numtexgens; i++)
    sb_set(sb, FI(FIN_TEX0 + static_cast<int>(i)), pi_ref(&in, plain.tex0 + static_cast<int>(i)));
  // Initialize other texture coordinates that are unused
  for (i = uid_data->genMode_numtexgens; i < 8; i++)
    sb_set(sb, FI(FIN_TEX0 + static_cast<int>(i)),
           sb_con(sb, sb->f[3], 3, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
#undef FI

  fout = rslc_b_local(sb->b, "frag_output", fout_t, 0, false);
  args[0] = sb_ref(sb, fin);
  args[1] = sb_ref(sb, fout);
  sb_do(sb, rslc_b_call(sb->b, process, args, 2));
  p.prev = rslc_b_local(sb->b, "prev", sb->i[4],
                        sb_bin(sb, RSLC_B_BAND, sb_mem(sb, sb_ref(sb, fout), FOUT_MAIN),
                               sb_i(sb, 255)),
                        false);

  const bool use_dual_source = !uid_data->no_dual_src || uid_data->blend_enable;
  // NOTE: Fragment may not be discarded if alpha test always fails and early depth test is enabled
  // (in this case we need to write a depth value if depth test passes regardless of the alpha
  // testing result)
  if (uid_data->Pretest == AlphaTestResult::Undetermined ||
      (uid_data->Pretest == AlphaTestResult::Fail && uid_data->ztest == EmulatedZ::Late))
  {
    static const enum rslc_b_binop logic[4] = {RSLC_B_AND, RSLC_B_OR, RSLC_B_NE, RSLC_B_EQ};
    uint32_t cond, then_s;
    cond = sb_bin(sb, logic[u32(uid_data->alpha_test_logic)],
                  pb_alpha_func(&p, uid_data->alpha_test_comp0, "r"),
                  pb_alpha_func(&p, uid_data->alpha_test_comp1, "g"));
    if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN))
      cond = sb_bin(sb, RSLC_B_EQ, cond, rslc_b_bool(sb->b, false));
    else
      cond = sb_un(sb, RSLC_B_NOT, cond);
    rslc_b_begin_block(sb->b);
    if (uid_data->uint_output)
      sb_set(sb, sb_ref(sb, ocol0),
             sb_con(sb, sb->u[4], 4, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
    else
      sb_set(sb, sb_ref(sb, ocol0), pb_fvec(sb, 4, 0.0));
    if (use_dual_source)
    {
      if (uid_data->uint_output)
        sb_set(sb, sb_ref(sb, ocol1),
               sb_con(sb, sb->u[4], 4, sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0), sb_i(sb, 0)));
      else
        sb_set(sb, sb_ref(sb, ocol1), pb_fvec(sb, 4, 0.0));
    }
    if (uid_data->per_pixel_depth)
      sb_set(sb, sb_bi(sb, "gl_FragDepth"),
             sb_f(sb, !g_backend_info.bSupportsReversedDepthRange ? 0.0 : 1.0));

    // ZCOMPLOC HACK:
    if (uid_data->ztest != EmulatedZ::EarlyWithZComplocHack)
    {
#ifdef __APPLE__
      if (uid_data->ztest == EmulatedZ::EarlyWithFBFetch)
      {
        // Instead of using discard, fetch the framebuffer's color value and use it as the output
        // for this fragment.
        sb_set(sb, sb_ref(sb, use_dual_source ? real_ocol0 : ocol0),
               sb_con(sb, sb->f[4], 2, sb_swz(sb, sb_ref(sb, initial), "xyz"), sb_f(sb, 1.0)));
        rslc_b_stmt(sb->b, rslc_b_return(sb->b, 0));
      }
      else
#endif
      {
        rslc_b_stmt(sb->b, rslc_b_discard(sb->b));
        if (a->api_type == APIType::D3D)
          rslc_b_stmt(sb->b, rslc_b_return(sb->b, 0));
      }
    }
    then_s = rslc_b_end_block(sb->b);
    rslc_b_stmt(sb->b, rslc_b_if(sb->b, cond, then_s, 0));
  }

  // Hardware testing indicates that an alpha of 1 can pass an alpha test,
  // but doesn't do anything in blending
  rslc_b_stmt(sb->b,
              rslc_b_if(sb->b, sb_bin(sb, RSLC_B_EQ, pb_swz(&p, p.prev, "a"), sb_i(sb, 1)),
                        pc_set_stmt(sb, pb_swz(&p, p.prev, "a"), sb_i(sb, 0)), 0));

#define PSU(k) sb_ref(sb, p.c.ps[k])
  if (uid_data->zfreeze)
  {
    struct rslc_var* screenpos = rslc_b_local(
        sb->b, "screenpos", sb->f[2],
        sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, rawpos), "xy"),
               sb_swz(sb, PSU(PSU_EFBSCALE), "xy")),
        false);
    // Opengl has reversed vertical screenspace coordinates
    if (a->api_type == APIType::OpenGL)
      sb_set(sb, sb_swz(sb, sb_ref(sb, screenpos), "y"),
             sb_bin(sb, RSLC_B_SUB, sb_f(sb, EFB_HEIGHT), sb_swz(sb, sb_ref(sb, screenpos), "y")));
    zcoord = rslc_b_local(
        sb->b, "zCoord", sb->i[1],
        sb_con(sb, sb->i[1], 1,
               sb_bin(sb, RSLC_B_ADD,
                      sb_bin(sb, RSLC_B_ADD, sb_swz(sb, PSU(PSU_ZSLOPE), "z"),
                             sb_bin(sb, RSLC_B_MUL, sb_swz(sb, PSU(PSU_ZSLOPE), "x"),
                                    sb_swz(sb, sb_ref(sb, screenpos), "x"))),
                      sb_bin(sb, RSLC_B_MUL, sb_swz(sb, PSU(PSU_ZSLOPE), "y"),
                             sb_swz(sb, sb_ref(sb, screenpos), "y")))),
        false);
  }
  else if (!host_config.fast_depth_calc)
  {
    // FastDepth means to trust the depth generated in perspective division.
    // It should be correct, but it seems not to be as accurate as required. TODO: Find out why!
    // For disabled FastDepth we just calculate the depth value again.
    // The performance impact of this additional calculation doesn't matter, but it prevents
    // the host GPU driver from performing any early depth test optimizations.
    // the screen space depth value = far z + (clip z / clip w) * z range
#define ZBIAS1(c_) sb_swz(sb, sb_idx(sb, PSU(PSU_ZBIAS), sb_i(sb, 1)), c_)
    zcoord = rslc_b_local(
        sb->b, "zCoord", sb->i[1],
        sb_bin(sb, RSLC_B_ADD, ZBIAS1("x"),
               sb_con(sb, sb->i[1], 1,
                      sb_bin(sb, RSLC_B_MUL,
                             sb_bin(sb, RSLC_B_DIV, sb_swz(sb, pi_ref(&in, plain.clipPos), "z"),
                                    sb_swz(sb, pi_ref(&in, plain.clipPos), "w")),
                             sb_con(sb, sb->f[1], 1, ZBIAS1("y"))))),
        false);
#undef ZBIAS1
  }
  else
  {
    uint32_t z = sb_swz(sb, sb_ref(sb, rawpos), "z");
    if (!host_config.backend_reversed_depth_range)
      z = sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), z);
    zcoord = rslc_b_local(
        sb->b, "zCoord", sb->i[1],
        sb_con(sb, sb->i[1], 1, sb_bin(sb, RSLC_B_MUL, z, sb_f(sb, 16777216.0))), false);
  }
  sb_set(sb, sb_ref(sb, zcoord),
         sb_call(sb, "clamp", 3, sb_ref(sb, zcoord), sb_i(sb, 0), sb_i(sb, 0xFFFFFF)));

  // depth texture can safely be ignored if the result won't be written to the depth buffer
  // (early_ztest) and isn't used for fog either
  const bool skip_ztexture = !uid_data->per_pixel_depth && uid_data->fog_fsel == FogType::Off;

  /* depth = 1.0 - float(zCoord) / 16777216.0, or float(zCoord) / 16777216.0 */
#define PS_WRITE_DEPTH()                                                                           \
  do                                                                                               \
  {                                                                                                \
    uint32_t d = sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[1], 1, sb_ref(sb, zcoord)),                 \
                        sb_f(sb, 16777216.0));                                                     \
    if (!host_config.backend_reversed_depth_range)                                                 \
      d = sb_bin(sb, RSLC_B_SUB, sb_f(sb, 1.0), d);                                                \
    sb_set(sb, sb_bi(sb, "gl_FragDepth"), d);                                                      \
  } while (0)
  // Note: z-textures are not written to depth buffer if early depth test is used
  const bool early_ztest = uid_data->ztest == EmulatedZ::Early ||
                           uid_data->ztest == EmulatedZ::EarlyWithFBFetch ||
                           uid_data->ztest == EmulatedZ::EarlyWithZComplocHack;
  if (uid_data->per_pixel_depth && early_ztest)
    PS_WRITE_DEPTH();

  // Note: depth texture output is only written to depth buffer if late depth test is used
  // theoretical final depth value is used for fog calculation, though, so we have to emulate
  // ztextures anyway
  if (uid_data->ztex_op != ZTexOp::Disabled && !skip_ztexture)
  {
    // use the texture input of the last texture stage, hopefully this has been read and
    // is in correct format...
    uint32_t dot[2], e;
    dot[0] = sb_swz(sb, sb_idx(sb, PSU(PSU_ZBIAS), sb_i(sb, 0)), "xyzw");
    dot[1] = sb_swz(sb, sb_mem(sb, sb_ref(sb, fout), FOUT_LAST_TEXTURE), "xyzw");
    e = sb_bin(sb, RSLC_B_ADD, rslc_b_call(sb->b, p.c.idot4, dot, 2),
               sb_swz(sb, sb_idx(sb, PSU(PSU_ZBIAS), sb_i(sb, 1)), "w"));
    if (uid_data->ztex_op == ZTexOp::Add)
      e = sb_bin(sb, RSLC_B_ADD, e, sb_ref(sb, zcoord));
    sb_set(sb, sb_ref(sb, zcoord), e);
    sb_set(sb, sb_ref(sb, zcoord),
           sb_bin(sb, RSLC_B_BAND, sb_ref(sb, zcoord), sb_i(sb, 0xFFFFFF)));
  }

  if (uid_data->per_pixel_depth && uid_data->ztest == EmulatedZ::Late)
    PS_WRITE_DEPTH();
#undef PS_WRITE_DEPTH

  // No dithering for RGB8 mode
  if (uid_data->dither)
  {
    // Flipper uses a standard 2x2 Bayer Matrix for 6 bit dithering
    struct rslc_var* dither = rslc_b_local(
        sb->b, "dither", sb->i[2],
        sb_bin(sb, RSLC_B_BAND,
               sb_con(sb, sb->i[2], 1, sb_swz(sb, sb_ref(sb, rawpos), "xy")), sb_i(sb, 1)),
        false);
    sb_set(sb, pb_swz(&p, p.prev, "rgb"),
           sb_bin(sb, RSLC_B_ADD,
                  sb_bin(sb, RSLC_B_ADD,
                         sb_bin(sb, RSLC_B_SUB, pb_swz(&p, p.prev, "rgb"),
                                sb_bin(sb, RSLC_B_SHR, pb_swz(&p, p.prev, "rgb"), sb_i(sb, 6))),
                         sb_bin(sb, RSLC_B_MUL,
                                sb_bin(sb, RSLC_B_BXOR, sb_swz(sb, sb_ref(sb, dither), "x"),
                                       sb_swz(sb, sb_ref(sb, dither), "y")),
                                sb_i(sb, 2))),
                  sb_swz(sb, sb_ref(sb, dither), "y")));
  }

  pb_fog(&p, rawpos, zcoord);

  if (uid_data->logic_op_enable)
  {
    uint32_t x = sb_bin(sb, RSLC_B_MUL, sb_ref(sb, initial), sb_f(sb, 255.0));
    struct rslc_var* fb = rslc_b_local(sb->b, "fb_value", sb->i[4],
                                       rslc_b_call(sb->b, p.c.iround[4], &x, 1), false);
    sb_set(sb, sb_ref(sb, p.prev),
           sb_bin(sb, RSLC_B_BAND, pb_logic_op(&p, uid_data->logic_op_mode, fb), sb_i(sb, 0xff)));
  }
  else if (uid_data->emulate_logic_op_with_blend)
  {
    switch (static_cast<LogicOp>(uid_data->logic_op_mode))
    {
    case LogicOp::Clear:
    case LogicOp::NoOp:
      sb_set(sb, sb_ref(sb, p.prev), pb_ivec(sb, 4, 0));
      break;
    case LogicOp::Copy:
      // Do nothing!
      break;
    case LogicOp::CopyInverted:
      sb_op_set(sb, RSLC_B_BXOR, sb_ref(sb, p.prev), sb_i(sb, 255));
      break;
    case LogicOp::Set:
    case LogicOp::Invert:  // In cooperation with blend
      sb_set(sb, sb_ref(sb, p.prev), pb_ivec(sb, 4, 255));
      break;
    default:
      break;
    }
  }

  // Write the color and alpha values to the framebuffer
  // If using shader blend, we still use the separate alpha
  // Some backends require the shader outputs be uint when writing to a uint render target for
  // logic op.
  if (uid_data->uint_output)
  {
    uint32_t e = sb_ref(sb, p.prev);
    if (uid_data->rgba6_format)
      e = sb_bin(sb, RSLC_B_BAND, e, sb_i(sb, 0xFC));
    sb_set(sb, sb_ref(sb, ocol0), sb_con(sb, sb->u[4], 1, e));
  }
  else
  {
    if (uid_data->rgba6_format)
      sb_set(sb, sb_swz(sb, sb_ref(sb, ocol0), "rgb"),
             sb_bin(sb, RSLC_B_DIV,
                    sb_con(sb, sb->f[3], 1,
                           sb_bin(sb, RSLC_B_SHR, pb_swz(&p, p.prev, "rgb"), sb_i(sb, 2))),
                    sb_f(sb, 63.0)));
    else
      sb_set(sb, sb_swz(sb, sb_ref(sb, ocol0), "rgb"),
             sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[3], 1, pb_swz(&p, p.prev, "rgb")),
                    sb_f(sb, 255.0)));

    // Colors will be blended against the 8-bit alpha from ocol1 and
    // the 6-bit alpha from ocol0 will be written to the framebuffer
    {
      uint32_t al = uid_data->useDstAlpha ? sb_swz(sb, PSU(PSU_ALPHA), "a") :
                                            pb_swz(&p, p.prev, "a");
      sb_set(sb, sb_swz(sb, sb_ref(sb, ocol0), "a"),
             sb_bin(sb, RSLC_B_DIV,
                    sb_con(sb, sb->f[1], 1, sb_bin(sb, RSLC_B_SHR, al, sb_i(sb, 2))),
                    sb_f(sb, 63.0)));
    }
    // Use dual-source color blending to perform dst alpha in a single pass
    if (use_dual_source)
      sb_set(sb, sb_ref(sb, ocol1),
             sb_con(sb, sb->f[4], 4, sb_f(sb, 0.0), sb_f(sb, 0.0), sb_f(sb, 0.0),
                    sb_bin(sb, RSLC_B_DIV, sb_con(sb, sb->f[1], 1, pb_swz(&p, p.prev, "a")),
                           sb_f(sb, 255.0))));
  }
#undef PSU

  if (uid_data->blend_enable)
  {
    struct rslc_var *src_color, *blend_src, *blend_dst, *blend_result;
    const enum rslc_b_binop rgb_op = uid_data->blend_subtract ? RSLC_B_SUB : RSLC_B_ADD;
    const enum rslc_b_binop a_op = uid_data->blend_subtract_alpha ? RSLC_B_SUB : RSLC_B_ADD;
    src_color = rslc_b_local(sb->b, "src_color", sb->f[4],
                             sb_ref(sb, uid_data->useDstAlpha ? ocol1 : ocol0), false);
    blend_src = rslc_b_local(sb->b, "blend_src", sb->f[4], 0, false);
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_src), "rgb"),
           pb_blend_factor(&p, u32(uid_data->blend_src_factor), false, false, initial, ocol0,
                           src_color));
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_src), "a"),
           pb_blend_factor(&p, u32(uid_data->blend_src_factor_alpha), false, true, initial,
                           ocol0, src_color));
    blend_dst = rslc_b_local(sb->b, "blend_dst", sb->f[4], 0, false);
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_dst), "rgb"),
           pb_blend_factor(&p, u32(uid_data->blend_dst_factor), true, false, initial, ocol0,
                           src_color));
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_dst), "a"),
           pb_blend_factor(&p, u32(uid_data->blend_dst_factor_alpha), true, true, initial,
                           ocol0, src_color));
    blend_result = rslc_b_local(sb->b, "blend_result", sb->f[4], 0, false);
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_result), "rgb"),
           sb_bin(sb, rgb_op,
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, initial), "rgb"),
                         sb_swz(sb, sb_ref(sb, blend_dst), "rgb")),
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, ocol0), "rgb"),
                         sb_swz(sb, sb_ref(sb, blend_src), "rgb"))));
    sb_set(sb, sb_swz(sb, sb_ref(sb, blend_result), "a"),
           sb_bin(sb, a_op,
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, initial), "a"),
                         sb_swz(sb, sb_ref(sb, blend_dst), "a")),
                  sb_bin(sb, RSLC_B_MUL, sb_swz(sb, sb_ref(sb, ocol0), "a"),
                         sb_swz(sb, sb_ref(sb, blend_src), "a"))));
    sb_set(sb, sb_ref(sb, real_ocol0), sb_ref(sb, blend_result));
  }
  else if (use_framebuffer_fetch)
  {
    sb_set(sb, sb_ref(sb, real_ocol0), sb_ref(sb, ocol0));
  }

  if (uid_data->bounding_box)
  {
    uint32_t x = sb_swz(sb, sb_ref(sb, rawpos), "xy");
    sb_do(sb, rslc_b_call(sb->b, p.c.update_bounding_box, &x, 1));
  }
  return rslc_b_func_end(sb->b);
}
