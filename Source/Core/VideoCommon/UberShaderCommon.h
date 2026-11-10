// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <string_view>

union ShaderHostConfig;
struct shader_build;
struct sb_bitfield;
struct rslc_var;
struct rslc_func;

namespace UberShader
{
/* A variable, one of its members (-1 for none) and a swizzle of it (nullptr for none),
 * made afresh as an expression each time it is read or written. */
struct sb_access
{
  struct rslc_var* var;
  int member;
  const char* swizzle;
};
uint32_t sb_access_ref(const struct shader_build* sb, const struct sb_access* a);

/* CalculateLighting(index, attnfunc, diffusefunc, pos, normal); lights is the uniforms'
 * clights */
struct rslc_func* BuildLightingFunction(const struct shader_build* sb, struct rslc_var* lights);
/* Each color channel's lighting; uniforms is the vertex uniforms' members by enum
 * shader_uniform */
struct VertexLightingBuild
{
  struct rslc_var* const* uniforms;
  const struct sb_bitfield* bitfield;
  struct rslc_func* calculate_lighting;
  struct sb_access world_pos, normal, in_color[2], out_color[2];
};
void BuildVertexLighting(const struct shader_build* sb, const struct VertexLightingBuild* l);
}  // namespace UberShader
