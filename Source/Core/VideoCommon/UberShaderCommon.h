// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

class ShaderCode;
enum class APIType;
union ShaderHostConfig;

namespace UberShader
{
// Vertex lighting
void WriteLightingFunction(ShaderCode& out);
void WriteVertexLighting(ShaderCode& out, APIType api_type, const char* world_pos_var,
                         const char* normal_var, const char* in_color_0_var,
                         const char* in_color_1_var, const char* out_color_0_var,
                         const char* out_color_1_var);
}  // namespace UberShader
