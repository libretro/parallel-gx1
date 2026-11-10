// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/LightingShaderGen.h"

#include <cstring>

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


/* ---- the same, built by calls ---- */

namespace
{
enum
{
  LIGHT_COLOR,
  LIGHT_COSATT,
  LIGHT_DISTATT,
  LIGHT_POS,
  LIGHT_DIR
};

struct light_build
{
  const struct shader_build* sb;
  const LightingUidData* uid_data;
  struct rslc_var* materials;
  struct rslc_var* lights;
  struct rslc_var *base_color, *pos, *normal;
  struct rslc_var *lacc, *ldir, *cos_attn, *dist_attn, *dist, *dist2, *attn, *mat;
};

/* clights[index].field */
uint32_t lb_light(const struct light_build* l, int index, int field)
{
  const struct shader_build* sb = l->sb;
  return sb_mem(sb, sb_idx(sb, sb_ref(sb, l->lights), sb_i(sb, index)),
                static_cast<uint32_t>(field));
}

/* cmtrl[index] */
uint32_t lb_material(const struct light_build* l, u32 index)
{
  return sb_idx(l->sb, sb_ref(l->sb, l->materials), sb_i(l->sb, static_cast<int32_t>(index)));
}

/* round(e * 255.0) */
uint32_t lb_round255(const struct light_build* l, uint32_t e)
{
  const struct shader_build* sb = l->sb;
  return sb_call(sb, "round", 1, sb_bin(sb, RSLC_B_MUL, e, sb_f(sb, 255.0)));
}

/* float3(1.0, a, a*a) */
uint32_t lb_poly(const struct light_build* l, uint32_t a, uint32_t b, uint32_t c)
{
  return sb_con(l->sb, l->sb->f[3], 3, sb_f(l->sb, 1.0), a, sb_bin(l->sb, RSLC_B_MUL, b, c));
}

void BuildLightShader(const struct light_build* l, int index, int litchan_index, bool alpha)
{
  const struct shader_build* sb = l->sb;
  const LightingUidData& uid_data = *l->uid_data;
  const char* swizzle = alpha ? "a" : "rgb";
  const unsigned n = alpha ? 1 : 3;
  uint32_t e;

  const auto attnfunc =
      static_cast<AttenuationFunc>((uid_data.attnfunc >> (2 * litchan_index)) & 0x3);
  const auto diffusefunc =
      static_cast<DiffuseFunc>((uid_data.diffusefunc >> (2 * litchan_index)) & 0x3);

#define LB(v) sb_ref(sb, l->v)
  switch (attnfunc)
  {
  case AttenuationFunc::None:
  case AttenuationFunc::Dir:
    sb_set(sb, LB(ldir),
           sb_call(sb, "normalize", 1,
                   sb_bin(sb, RSLC_B_SUB, sb_swz(sb, lb_light(l, index, LIGHT_POS), "xyz"),
                          sb_swz(sb, LB(pos), "xyz"))));
    sb_set(sb, LB(attn), sb_f(sb, 1.0));
    rslc_b_stmt(sb->b, rslc_b_if(sb->b,
                                 sb_bin(sb, RSLC_B_EQ, sb_call(sb, "length", 1, LB(ldir)),
                                        sb_f(sb, 0.0)),
                                 rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, LB(ldir),
                                                                  LB(normal))),
                                 0));
    break;
  case AttenuationFunc::Spec:
    sb_set(sb, LB(ldir),
           sb_call(sb, "normalize", 1,
                   sb_bin(sb, RSLC_B_SUB, sb_swz(sb, lb_light(l, index, LIGHT_POS), "xyz"),
                          sb_swz(sb, LB(pos), "xyz"))));
    sb_set(sb, LB(attn),
           rslc_b_select(sb->b,
                         sb_bin(sb, RSLC_B_GE, sb_call(sb, "dot", 2, LB(normal), LB(ldir)),
                                sb_f(sb, 0.0)),
                         sb_call(sb, "max", 2, sb_f(sb, 0.0),
                                 sb_call(sb, "dot", 2, LB(normal),
                                         sb_swz(sb, lb_light(l, index, LIGHT_DIR), "xyz"))),
                         sb_f(sb, 0.0)));
    sb_set(sb, LB(cos_attn), sb_swz(sb, lb_light(l, index, LIGHT_COSATT), "xyz"));
    e = sb_swz(sb, lb_light(l, index, LIGHT_DISTATT), "xyz");
    if (diffusefunc != DiffuseFunc::None)
      e = sb_call(sb, "normalize", 1, e);
    sb_set(sb, LB(dist_attn), e);
    sb_set(sb, LB(attn),
           sb_bin(sb, RSLC_B_DIV,
                  sb_call(sb, "max", 2, sb_f(sb, 0.0),
                          sb_call(sb, "dot", 2, LB(cos_attn),
                                  lb_poly(l, LB(attn), LB(attn), LB(attn)))),
                  sb_call(sb, "dot", 2, LB(dist_attn), lb_poly(l, LB(attn), LB(attn), LB(attn)))));
    break;
  case AttenuationFunc::Spot:
    sb_set(sb, LB(ldir),
           sb_bin(sb, RSLC_B_SUB, sb_swz(sb, lb_light(l, index, LIGHT_POS), "xyz"),
                  sb_swz(sb, LB(pos), "xyz")));
    sb_set(sb, LB(dist2), sb_call(sb, "dot", 2, LB(ldir), LB(ldir)));
    sb_set(sb, LB(dist), sb_call(sb, "sqrt", 1, LB(dist2)));
    sb_set(sb, LB(ldir), sb_bin(sb, RSLC_B_DIV, LB(ldir), LB(dist)));
    sb_set(sb, LB(attn),
           sb_call(sb, "max", 2, sb_f(sb, 0.0),
                   sb_call(sb, "dot", 2, LB(ldir),
                           sb_swz(sb, lb_light(l, index, LIGHT_DIR), "xyz"))));
    // attn*attn may overflow
    e = sb_bin(sb, RSLC_B_ADD,
               sb_bin(sb, RSLC_B_ADD, sb_swz(sb, lb_light(l, index, LIGHT_COSATT), "x"),
                      sb_bin(sb, RSLC_B_MUL, sb_swz(sb, lb_light(l, index, LIGHT_COSATT), "y"),
                             LB(attn))),
               sb_bin(sb, RSLC_B_MUL,
                      sb_bin(sb, RSLC_B_MUL, sb_swz(sb, lb_light(l, index, LIGHT_COSATT), "z"),
                             LB(attn)),
                      LB(attn)));
    sb_set(sb, LB(attn),
           sb_bin(sb, RSLC_B_DIV, sb_call(sb, "max", 2, sb_f(sb, 0.0), e),
                  sb_call(sb, "dot", 2, sb_swz(sb, lb_light(l, index, LIGHT_DISTATT), "xyz"),
                          sb_con(sb, sb->f[3], 3, sb_f(sb, 1.0), LB(dist), LB(dist2)))));
    break;
  }

  /* float3(clights[index].color.rgb), or float(...a) */
  e = sb_con(sb, sb->f[n], 1, sb_swz(sb, lb_light(l, index, LIGHT_COLOR), swizzle));
  switch (diffusefunc)
  {
  case DiffuseFunc::None:
    e = sb_bin(sb, RSLC_B_MUL, LB(attn), e);
    break;
  case DiffuseFunc::Sign:
    e = sb_bin(sb, RSLC_B_MUL,
               sb_bin(sb, RSLC_B_MUL, LB(attn), sb_call(sb, "dot", 2, LB(ldir), LB(normal))), e);
    break;
  case DiffuseFunc::Clamp:
    e = sb_bin(sb, RSLC_B_MUL,
               sb_bin(sb, RSLC_B_MUL, LB(attn),
                      sb_call(sb, "max", 2, sb_f(sb, 0.0),
                              sb_call(sb, "dot", 2, LB(ldir), LB(normal)))),
               e);
    break;
  default:
    ASSERT(false);
    return;
  }
  sb_op_set(sb, RSLC_B_ADD, sb_swz(sb, LB(lacc), swizzle),
            sb_con(sb, sb->i[n], 1, sb_call(sb, "round", 1, e)));
#undef LB
}
}  // namespace

void BuildLightingShaderHeader(const struct shader_build* sb, const LightingUidData& uid_data,
                               struct rslc_var* materials, struct rslc_var* lights,
                               struct rslc_func** chn)
{
  static const char* const fn_names[NUM_XF_COLOR_CHANNELS] = {"dolphin_calculate_lighting_chn0",
                                                              "dolphin_calculate_lighting_chn1"};
  static const char* const param_names[3] = {"base_color", "pos", "_normal"};
  static const unsigned dirs[3] = {RSLC_B_IN, RSLC_B_IN, RSLC_B_IN};
  const struct rslc_type* param_types[3];
  struct light_build l;
  u32 j;

  param_types[0] = sb->f[4];
  param_types[1] = sb->f[3];
  param_types[2] = sb->f[3];
  memset(&l, 0, sizeof(l));
  l.sb = sb;
  l.uid_data = &uid_data;
  l.materials = materials;
  l.lights = lights;

  for (j = 0; j < NUM_XF_COLOR_CHANNELS; j++)
  {
    const bool colormatsource = !!(uid_data.matsource & (1 << j));
    const bool alphamatsource = !!(uid_data.matsource & (1 << (j + 2)));
    struct rslc_func* f = rslc_b_func(sb->b, fn_names[j], sb->f[4], 3, param_types, dirs,
                                      param_names);
    uint32_t e;
    int i;
    chn[j] = f;
    rslc_b_func_begin(sb->b, f);
    l.base_color = rslc_b_param(f, 0);
    l.pos = rslc_b_param(f, 1);
    l.normal = rslc_b_param(f, 2);

#define LB(v) sb_ref(sb, l.v)
    l.lacc = rslc_b_local(sb->b, "lacc", sb->i[4], 0, false);
    l.ldir = rslc_b_local(sb->b, "ldir", sb->f[3], 0, false);
    rslc_b_local(sb->b, "h", sb->f[3], 0, false);
    l.cos_attn = rslc_b_local(sb->b, "cosAttn", sb->f[3], 0, false);
    l.dist_attn = rslc_b_local(sb->b, "distAttn", sb->f[3], 0, false);
    l.dist = rslc_b_local(sb->b, "dist", sb->f[1], 0, false);
    l.dist2 = rslc_b_local(sb->b, "dist2", sb->f[1], 0, false);
    l.attn = rslc_b_local(sb->b, "attn", sb->f[1], 0, false);

    if (colormatsource)  // from vertex
      e = sb_con(sb, sb->i[4], 1, lb_round255(&l, LB(base_color)));
    else  // from color
      e = lb_material(&l, j + 2);
    l.mat = rslc_b_local(sb->b, "mat", sb->i[4], e, false);

    if ((uid_data.enablelighting & (1 << j)) != 0)
    {
      if ((uid_data.ambsource & (1 << j)) != 0)  // from vertex
        sb_set(sb, LB(lacc), sb_con(sb, sb->i[4], 1, lb_round255(&l, LB(base_color))));
      else  // from color
        sb_set(sb, LB(lacc), lb_material(&l, j));
    }
    else
    {
      sb_set(sb, LB(lacc),
             sb_con(sb, sb->i[4], 4, sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255), sb_i(sb, 255)));
    }

    // check if alpha is different
    if (alphamatsource != colormatsource)
    {
      if (alphamatsource)  // from vertex
        sb_set(sb, sb_swz(sb, LB(mat), "w"),
               sb_con(sb, sb->i[1], 1, lb_round255(&l, sb_swz(sb, LB(base_color), "w"))));
      else  // from color
        sb_set(sb, sb_swz(sb, LB(mat), "w"), sb_swz(sb, lb_material(&l, j + 2), "w"));
    }

    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)
    {
      if ((uid_data.ambsource & (1 << (j + 2))) != 0)  // from vertex
        sb_set(sb, sb_swz(sb, LB(lacc), "w"),
               sb_con(sb, sb->i[1], 1, lb_round255(&l, sb_swz(sb, LB(base_color), "w"))));
      else  // from color
        sb_set(sb, sb_swz(sb, LB(lacc), "w"), sb_swz(sb, lb_material(&l, j), "w"));
    }
    else
    {
      sb_set(sb, sb_swz(sb, LB(lacc), "w"), sb_i(sb, 255));
    }

    if ((uid_data.enablelighting & (1 << j)) != 0)  // Color lights
    {
      for (i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * j))) != 0)
          BuildLightShader(&l, i, static_cast<int>(j), false);
      }
    }
    if ((uid_data.enablelighting & (1 << (j + 2))) != 0)  // Alpha lights
    {
      for (i = 0; i < 8; ++i)
      {
        if ((uid_data.light_mask & (1 << (i + 8 * (j + 2)))) != 0)
          BuildLightShader(&l, i, static_cast<int>(j) + 2, true);
      }
    }
    sb_set(sb, LB(lacc), sb_call(sb, "clamp", 3, LB(lacc), sb_i(sb, 0), sb_i(sb, 255)));
    rslc_b_stmt(
        sb->b,
        rslc_b_return(
            sb->b,
            sb_bin(sb, RSLC_B_DIV,
                   sb_con(sb, sb->f[4], 1,
                          sb_bin(sb, RSLC_B_SHR,
                                 sb_bin(sb, RSLC_B_MUL, LB(mat),
                                        sb_bin(sb, RSLC_B_ADD, LB(lacc),
                                               sb_bin(sb, RSLC_B_SHR, LB(lacc), sb_i(sb, 7)))),
                                 sb_i(sb, 8))),
                   sb_f(sb, 255.0))));
#undef LB
    rslc_b_func_end(sb->b);
  }
}
