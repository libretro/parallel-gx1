// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "Common/CommonTypes.h"

class ShaderCode;
struct shader_build;
struct rslc_var;
struct rslc_func;

/**
 * Common uid data used for shader generators that use lighting calculations.
 */
struct LightingUidData
{
  u32 matsource : 4;       // 4x1 bit
  u32 enablelighting : 4;  // 4x1 bit
  u32 ambsource : 4;       // 4x1 bit
  u32 diffusefunc : 8;     // 4x2 bits
  u32 attnfunc : 8;        // 4x2 bits
  u32 light_mask : 32;     // 4x8 bits
};

constexpr char s_lighting_struct[] = "struct Light {\n"
                                     "\tint4 color;\n"
                                     "\tfloat4 cosatt;\n"
                                     "\tfloat4 distatt;\n"
                                     "\tfloat4 pos;\n"
                                     "\tfloat4 dir;\n"
                                     "};\n";

void GenerateLightingShaderHeader(ShaderCode& object, const LightingUidData& uid_data);
/* GenerateLightingShaderHeader's dolphin_calculate_lighting_chn0 and _chn1, into chn; materials
 * and lights the uniforms' members */
void BuildLightingShaderHeader(const struct shader_build* sb, const LightingUidData& uid_data,
                               struct rslc_var* materials, struct rslc_var* lights,
                               struct rslc_func** chn);
void GetLightingShaderUid(LightingUidData& uid_data);
