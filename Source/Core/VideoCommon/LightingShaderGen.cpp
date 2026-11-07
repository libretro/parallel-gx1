// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/LightingShaderGen.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/XFMemory.h"

static void GenerateLightShader(ShaderCode& object, const LightingUidData& uid_data, int index,
                                int litchan_index, bool alpha)
{
  const char* swizzle = alpha ? "a" : "rgb";
  const char* swizzle_components = (alpha) ? "" : "3";

  const auto attnfunc =
      static_cast<AttenuationFunc>((uid_data.attnfunc >> (2 * litchan_index)) & 0x3);
  const auto diffusefunc =
      static_cast<DiffuseFunc>((uid_data.diffusefunc >> (2 * litchan_index)) & 0x3);

  switch (attnfunc)
  {
  case AttenuationFunc::None:
  case AttenuationFunc::Dir:
    object.Printf("ldir = normalize(%s[%d].pos.xyz - pos.xyz);\n", (I_LIGHTS), (index));
    object.Puts("attn = 1.0;\n");
    object.Puts("if (length(ldir) == 0.0)\n"
              "\t ldir = _normal;\n");
    break;
  case AttenuationFunc::Spec:
    object.Printf("ldir = normalize(%s[%d].pos.xyz - pos.xyz);\n", (I_LIGHTS), (index));
    object.Printf("attn = (dot(_normal, ldir) >= 0.0) ? max(0.0, dot(_normal, %s[%d].dir.xyz)) : 0.0;\n", (I_LIGHTS), (index));
    object.Printf("cosAttn = %s[%d].cosatt.xyz;\n", (I_LIGHTS), (index));
    object.Printf("distAttn = %s(%s[%d].distatt.xyz);\n", (diffusefunc == DiffuseFunc::None) ? "" : "normalize", (I_LIGHTS), (index));
    object.Puts("attn = max(0.0f, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, float3(1.0, attn, attn*attn));\n");
    break;
  case AttenuationFunc::Spot:
    object.Printf("ldir = %s[%d].pos.xyz - pos.xyz;\n", (I_LIGHTS), (index));
    object.Printf("dist2 = dot(ldir, ldir);\n"
              "dist = sqrt(dist2);\n"
              "ldir = ldir / dist;\n"
              "attn = max(0.0, dot(ldir, %s[%d].dir.xyz));\n", (I_LIGHTS), (index));
    // attn*attn may overflow
    object.Printf("attn = max(0.0, %s[%d].cosatt.x + %s[%d].cosatt.y*attn + %s[%d].cosatt.z*attn*attn) / dot(%s[%d].distatt.xyz, float3(1.0,dist,dist2));\n", (I_LIGHTS), (index), (I_LIGHTS), (index), (I_LIGHTS), (index), (I_LIGHTS), (index));
    break;
  }

  switch (diffusefunc)
  {
  case DiffuseFunc::None:
    object.Printf("lacc.%s += int%s(round(attn * float%s(%s[%d].color.%s)));\n", swizzle, swizzle_components, swizzle_components, (I_LIGHTS), (index), (swizzle));
    break;
  case DiffuseFunc::Sign:
  case DiffuseFunc::Clamp:
    object.Printf("lacc.%s += int%s(round(attn * %sdot(ldir, _normal)) * float%s(%s[%d].color.%s)));\n", swizzle, swizzle_components, diffusefunc != DiffuseFunc::Sign ? "max(0.0," : "(", swizzle_components, (I_LIGHTS), (index), (swizzle));
    break;
  default:
    ASSERT(false);
  }

  object.Puts("\n");
}

// vertex shader
// lights/colors
// materials name is I_MATERIALS in vs and I_PMATERIALS in ps
void GenerateLightingShaderHeader(ShaderCode& object, const LightingUidData& uid_data)
{
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    object.Printf("vec4 dolphin_calculate_lighting_chn%u(vec4 base_color, vec3 pos, vec3 _normal)\n", j);
    object.Puts("{\n");

    object.Puts("\tint4 lacc;\n"
              "\tvec3 ldir, h, cosAttn, distAttn;\n"
              "\tfloat dist, dist2, attn;\n");

    const bool colormatsource = !!(uid_data.matsource & (1 << j));
    if (colormatsource)  // from vertex
      object.Puts("\tint4 mat = int4(round(base_color * 255.0));\n");
    else  // from color
      object.Printf("\tint4 mat = %s[%u];\n", I_MATERIALS, j + 2);

    if ((uid_data.enablelighting & (1 << j)) != 0)
    {
      if ((uid_data.ambsource & (1 << j)) != 0)  // from vertex
        object.Puts("\tlacc = int4(round(base_color * 255.0));\n");
      else  // from color
        object.Printf("\tlacc = %s[%u];\n", I_MATERIALS, j);
    }
    else
    {
      object.Puts("\tlacc = int4(255, 255, 255, 255);\n");
    }

    // check if alpha is different
    const bool alphamatsource = !!(uid_data.matsource & (1 << (j + 2)));
    if (alphamatsource != colormatsource)
    {
      if (alphamatsource)  // from vertex
        object.Puts("\tmat.w = int(round(base_color.w * 255.0));\n");
      else  // from color
        object.Printf("\tmat.w = %s[%u].w;\n", I_MATERIALS, j + 2);
    }

    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)
    {
      if ((uid_data.ambsource & (1 << (j + 2))) != 0)  // from vertex
        object.Puts("\tlacc.w = int(round(base_color.w * 255.0));\n");
      else  // from color
        object.Printf("\tlacc.w = %s[%u].w;\n", I_MATERIALS, j);
    }
    else
    {
      object.Puts("\tlacc.w = 255;\n");
    }

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * j))) != 0)
          GenerateLightShader(object, uid_data, i, j, false);
      }
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      for (int i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * (j + 2)))) != 0)
          GenerateLightShader(object, uid_data, i, j + 2, true);
      }
    }
    object.Puts("\tlacc = clamp(lacc, 0, 255);\n");
    object.Puts("\treturn vec4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n");
    object.Puts("}\n"
              "\n");
  }
}

void GetLightingShaderUid(LightingUidData& uid_data)
{
  for (u32 j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    uid_data.matsource |= static_cast<u32>(xfmem.color[j].matsource.Value()) << j;
    uid_data.matsource |= static_cast<u32>(xfmem.alpha[j].matsource.Value()) << (j + 2);
    uid_data.enablelighting |= xfmem.color[j].enablelighting << j;
    uid_data.enablelighting |= xfmem.alpha[j].enablelighting << (j + 2);

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      uid_data.ambsource |= static_cast<u32>(xfmem.color[j].ambsource.Value()) << j;
      uid_data.attnfunc |= static_cast<u32>(xfmem.color[j].attnfunc.Value()) << (2 * j);
      uid_data.diffusefunc |= static_cast<u32>(xfmem.color[j].diffusefunc.Value()) << (2 * j);
      uid_data.light_mask |= xfmem.color[j].GetFullLightMask() << (8 * j);
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      uid_data.ambsource |= static_cast<u32>(xfmem.alpha[j].ambsource.Value()) << (j + 2);
      uid_data.attnfunc |= static_cast<u32>(xfmem.alpha[j].attnfunc.Value()) << (2 * (j + 2));
      uid_data.diffusefunc |= static_cast<u32>(xfmem.alpha[j].diffusefunc.Value()) << (2 * (j + 2));
      uid_data.light_mask |= xfmem.alpha[j].GetFullLightMask() << (8 * (j + 2));
    }
  }
}

