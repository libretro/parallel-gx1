// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

enum class EFBReinterpretType;
enum class TextureFormat;

struct shader_build;

namespace FramebufferShaderGen
{
/* Stages built by calls: for AbstractGfx::CreateShaderFromBuild. */
bool BuildScreenQuadVertexShader(struct shader_build* sb, const void* arg);
/* arg: u32[2], the texture and color varyings */
bool BuildPassthroughGeometryShader(struct shader_build* sb, const void* arg);
bool BuildTextureCopyVertexShader(struct shader_build* sb, const void* arg);
bool BuildTextureCopyPixelShader(struct shader_build* sb, const void* arg);
/* arg: u32, the samples */
bool BuildResolveColorPixelShader(struct shader_build* sb, const void* arg);
bool BuildResolveDepthPixelShader(struct shader_build* sb, const void* arg);
bool BuildClearVertexShader(struct shader_build* sb, const void* arg);
bool BuildEFBPokeVertexShader(struct shader_build* sb, const void* arg);
bool BuildColorPixelShader(struct shader_build* sb, const void* arg);
struct FormatConversionArgs
{
  EFBReinterpretType convtype;
  u32 samples;
};
bool BuildFormatConversionShader(struct shader_build* sb, const void* arg);
/* arg: TextureFormat[2], from and to; false for a pair there is no shader for */
bool BuildTextureReinterpretShader(struct shader_build* sb, const void* arg);
bool BuildEFBRestorePixelShader(struct shader_build* sb, const void* arg);


}  // namespace FramebufferShaderGen
