// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class EFBReinterpretType;
enum class TextureFormat;

namespace FramebufferShaderGen
{
ShaderCode GenerateScreenQuadVertexShader();
ShaderCode GeneratePassthroughGeometryShader(u32 num_tex, u32 num_colors);
ShaderCode GenerateTextureCopyVertexShader();
ShaderCode GenerateTextureCopyPixelShader();
ShaderCode GenerateResolveColorPixelShader(u32 samples);
ShaderCode GenerateResolveDepthPixelShader(u32 samples);
ShaderCode GenerateClearVertexShader();
ShaderCode GenerateEFBPokeVertexShader();
ShaderCode GenerateColorPixelShader();
ShaderCode GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples);
ShaderCode GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format);
ShaderCode GenerateEFBRestorePixelShader();

}  // namespace FramebufferShaderGen
