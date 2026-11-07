// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/UberShaderCommon.h"

#include <utility>

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
void WriteLightingFunction(ShaderCode& out)
{
  // ==============================================
  // Lighting channel calculation helper
  // ==============================================
  out.Puts("int4 CalculateLighting(uint index, uint attnfunc, uint diffusefunc, float3 pos, float3 normal) {\n"
            "  float3 ldir, h, cosAttn, distAttn;\n"
            "  float dist, dist2, attn;\n"
            "\n"
            "  switch (attnfunc) {\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(AttenuationFunc::None), ShaderEnumName(AttenuationFunc::None));
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(AttenuationFunc::Dir), ShaderEnumName(AttenuationFunc::Dir));
  out.Puts("    ldir = normalize("
            I_LIGHTS
            "[index].pos.xyz - pos.xyz);\n"
            "    attn = 1.0;\n"
            "    if (length(ldir) == 0.0)\n"
            "      ldir = normal;\n"
            "    break;\n"
            "\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(AttenuationFunc::Spec), ShaderEnumName(AttenuationFunc::Spec));
  out.Puts("    ldir = normalize("
            I_LIGHTS
            "[index].pos.xyz - pos.xyz);\n"
            "    attn = (dot(normal, ldir) >= 0.0) ? max(0.0, dot(normal, "
            I_LIGHTS
            "[index].dir.xyz)) : 0.0;\n"
            "    cosAttn = "
            I_LIGHTS
            "[index].cosatt.xyz;\n");
  out.Printf("    if (diffusefunc == 0x%xu /* %s */)\n", ShaderEnumValue(DiffuseFunc::None), ShaderEnumName(DiffuseFunc::None));
  out.Puts("      distAttn = "
            I_LIGHTS
            "[index].distatt.xyz;\n"
            "    else\n"
            "      distAttn = normalize("
            I_LIGHTS
            "[index].distatt.xyz);\n"
            "    attn = max(0.0, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, float3(1.0, attn, attn*attn));\n"
            "    break;\n"
            "\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(AttenuationFunc::Spot), ShaderEnumName(AttenuationFunc::Spot));
  out.Puts("    ldir = "
            I_LIGHTS
            "[index].pos.xyz - pos.xyz;\n"
            "    dist2 = dot(ldir, ldir);\n"
            "    dist = sqrt(dist2);\n"
            "    ldir = ldir / dist;\n"
            "    attn = max(0.0, dot(ldir, "
            I_LIGHTS
            "[index].dir.xyz));\n"
            "    attn = max(0.0, "
            I_LIGHTS
            "[index].cosatt.x + "
            I_LIGHTS
            "[index].cosatt.y * attn + "
            I_LIGHTS
            "[index].cosatt.z * attn * attn) / dot("
            I_LIGHTS
            "[index].distatt.xyz, float3(1.0, dist, dist2));\n"
            "    break;\n"
            "\n");
  out.Puts("  default:\n"
            "    attn = 1.0;\n"
            "    ldir = normal;\n"
            "    break;\n"
            "  }\n"
            "\n"
            "  switch (diffusefunc) {\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(DiffuseFunc::None), ShaderEnumName(DiffuseFunc::None));
  out.Puts("    return int4(round(attn * float4("
            I_LIGHTS
            "[index].color)));\n"
            "\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(DiffuseFunc::Sign), ShaderEnumName(DiffuseFunc::Sign));
  out.Puts("    return int4(round(attn * dot(ldir, normal) * float4("
            I_LIGHTS
            "[index].color)));\n"
            "\n");
  out.Printf("  case 0x%xu /* %s */:\n", ShaderEnumValue(DiffuseFunc::Clamp), ShaderEnumName(DiffuseFunc::Clamp));
  out.Puts("    return int4(round(attn * max(0.0, dot(ldir, normal)) * float4("
            I_LIGHTS
            "[index].color)));\n"
            "\n");
  out.Puts("  default:\n"
            "    return int4(0, 0, 0, 0);\n"
            "  }\n"
            "}\n"
            "\n");
}

void WriteVertexLighting(ShaderCode& out, APIType api_type, const char* world_pos_var,
                         const char* normal_var, const char* in_color_0_var,
                         const char* in_color_1_var, const char* out_color_0_var,
                         const char* out_color_1_var)
{
  out.Puts("// Lighting\n");
  out.Printf("for (uint chan = 0u; chan < %uu; chan++) {\n", (unsigned)(NUM_XF_COLOR_CHANNELS));
  out.Puts("  uint colorreg = xfmem_color(chan);\n"
            "  uint alphareg = xfmem_alpha(chan);\n"
            "  int4 mat = "
            I_MATERIALS
            "[chan + 2u]; \n"
            "  int4 lacc = int4(255, 255, 255, 255);\n"
            "\n");

  out.Printf("  if (%s != 0u)\n", BitfieldExtract<&LitChannel::matsource>("colorreg").text);
  out.Printf("    mat.xyz = int3(round(((chan == 0u) ? %s.xyz : %s.xyz) * 255.0));\n", in_color_0_var, in_color_1_var);

  out.Printf("  if (%s != 0u)\n", BitfieldExtract<&LitChannel::matsource>("alphareg").text);
  out.Printf("    mat.w = int(round(((chan == 0u) ? %s.w : %s.w) * 255.0));\n", in_color_0_var, in_color_1_var);
  out.Puts("  else\n"
            "    mat.w = "
            I_MATERIALS
            " [chan + 2u].w;\n"
            "\n");

  out.Printf("  if (%s != 0u) {\n", BitfieldExtract<&LitChannel::enablelighting>("colorreg").text);
  out.Printf("    if (%s != 0u)\n", BitfieldExtract<&LitChannel::ambsource>("colorreg").text);
  out.Printf("      lacc.xyz = int3(round(((chan == 0u) ? %s.xyz : %s.xyz) * 255.0));\n", in_color_0_var, in_color_1_var);
  out.Puts("    else\n"
            "      lacc.xyz = "
            I_MATERIALS
            " [chan].xyz;\n"
            "\n");
  out.Printf("    uint light_mask = %s | (%s << 4u);\n", BitfieldExtract<&LitChannel::lightMask0_3>("colorreg").text, BitfieldExtract<&LitChannel::lightMask4_7>("colorreg").text);
  out.Printf("    uint attnfunc = %s;\n", BitfieldExtract<&LitChannel::attnfunc>("colorreg").text);
  out.Printf("    uint diffusefunc = %s;\n", BitfieldExtract<&LitChannel::diffusefunc>("colorreg").text);
  out.Printf("    for (uint light_index = 0u; light_index < 8u; light_index++) {\n"
            "      if ((light_mask & (1u << light_index)) != 0u)\n"
            "        lacc.xyz += CalculateLighting(light_index, attnfunc, diffusefunc, %s, %s).xyz;\n", world_pos_var, normal_var);
  out.Puts("    }\n"
            "  }\n"
            "\n");

  out.Printf("  if (%s != 0u) {\n", BitfieldExtract<&LitChannel::enablelighting>("alphareg").text);
  out.Printf("    if (%s != 0u) {\n", BitfieldExtract<&LitChannel::ambsource>("alphareg").text);
  out.Printf("      if ((components & (%uu << chan)) != 0u) // VB_HAS_COL0\n", std::to_underlying(VB_HAS_COL0));
  out.Printf("        lacc.w = int(round(((chan == 0u) ? %s.w : %s.w) * 255.0));\n", in_color_0_var, in_color_1_var);
  out.Printf("      else if ((components & %uu) != 0u) // VB_HAS_COLO0\n", std::to_underlying(VB_HAS_COL0));
  out.Printf("        lacc.w = int(round(%s.w * 255.0));\n", in_color_0_var);
  out.Puts("      else\n"
            "        lacc.w = 255;\n"
            "    } else {\n"
            "      lacc.w = "
            I_MATERIALS
            " [chan].w;\n"
            "    }\n"
            "\n");
  out.Printf("    uint light_mask = %s | (%s << 4u);\n", BitfieldExtract<&LitChannel::lightMask0_3>("alphareg").text, BitfieldExtract<&LitChannel::lightMask4_7>("alphareg").text);
  out.Printf("    uint attnfunc = %s;\n", BitfieldExtract<&LitChannel::attnfunc>("alphareg").text);
  out.Printf("    uint diffusefunc = %s;\n", BitfieldExtract<&LitChannel::diffusefunc>("alphareg").text);
  out.Printf("    for (uint light_index = 0u; light_index < 8u; light_index++) {\n"
            "\n"
            "      if ((light_mask & (1u << light_index)) != 0u)\n"
            "\n"
            "        lacc.w += CalculateLighting(light_index, attnfunc, diffusefunc, %s, %s).w;\n", world_pos_var, normal_var);
  out.Puts("    }\n"
            "  }\n"
            "\n");

  out.Printf("  lacc = clamp(lacc, 0, 255);\n"
            "\n"
            "  // Hopefully GPUs that can support dynamic indexing will optimize this.\n"
            "  float4 lit_color = float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n"
            "  switch (chan) {\n"
            "  case 0u: %s = lit_color; break;\n", out_color_0_var);
  out.Printf("  case 1u: %s = lit_color; break;\n", out_color_1_var);
  out.Puts("  }\n"
            "}\n"
            "\n");
}
}  // namespace UberShader
