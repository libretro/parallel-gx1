// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <string_view>

class ShaderCode;
enum class APIType;
union ShaderHostConfig;
struct shader_build;
struct sb_bitfield;
struct rslc_var;
struct rslc_func;

namespace UberShader
{
// Vertex lighting
void WriteLightingFunction(ShaderCode& out);
void WriteVertexLighting(ShaderCode& out, APIType api_type, const char* world_pos_var,
                         const char* normal_var, const char* in_color_0_var,
                         const char* in_color_1_var, const char* out_color_0_var,
                         const char* out_color_1_var);

/* A variable, one of its members (-1 for none) and a swizzle of it (nullptr for none),
 * made afresh as an expression each time it is read or written. */
struct sb_access
{
  struct rslc_var* var;
  int member;
  const char* swizzle;
};
uint32_t sb_access_ref(const struct shader_build* sb, const struct sb_access* a);

/* WriteLightingFunction's CalculateLighting; lights is the uniforms' clights */
struct rslc_func* BuildLightingFunction(const struct shader_build* sb, struct rslc_var* lights);
/* WriteVertexLighting; uniforms is the vertex uniforms' members by enum shader_uniform */
struct VertexLightingBuild
{
  struct rslc_var* const* uniforms;
  const struct sb_bitfield* bitfield;
  struct rslc_func* calculate_lighting;
  struct sb_access world_pos, normal, in_color[2], out_color[2];
};
void BuildVertexLighting(const struct shader_build* sb, const struct VertexLightingBuild* l);
}  // namespace UberShader
