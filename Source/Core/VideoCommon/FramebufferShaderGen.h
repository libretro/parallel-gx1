// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

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

ShaderCode GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples);
ShaderCode GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format);
ShaderCode GenerateEFBRestorePixelShader();

}  // namespace FramebufferShaderGen
