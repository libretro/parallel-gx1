// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureDecoder.h"

enum class APIType;

namespace TextureConversionShaderGen
{
/* An EFB copy to VRAM pixel shader reads what it copies and how from its uniforms (copy_flags),
 * each branch on them taking the path a shader built for that copy alone would. One is built for
 * each format; the one for none takes the format from them as well, and draws every copy the
 * same while the one for its format is not built yet. */
enum
{
  COPY_FLAG_ALL_FILTER_COEFS = 1u << 0,
  COPY_FLAG_EFB_HAS_ALPHA = 1u << 1,
  COPY_FLAG_FILTER_CAN_OVERFLOW = 1u << 2,
  COPY_FLAG_APPLY_GAMMA = 1u << 3,
  COPY_FLAG_DEPTH = 1u << 4,
  COPY_FLAG_INTENSITY = 1u << 5
  /* from bit 8, how the format takes each channel (GetCopyFlags) */
};

ShaderCode GenerateVertexShader(APIType api_type);
ShaderCode GeneratePixelShader(APIType api_type, const EFBCopyFormat* format);

/* The format a copy into dst_format is drawn in: with true colour forced, a wider one. */
EFBCopyFormat GetDrawnFormat(EFBCopyFormat dst_format);
/* copy_flags for a copy */
u32 GetCopyFlags(EFBCopyFormat dst_format, bool is_depth_copy, bool is_intensity, float gamma_rcp,
                 const std::array<u32, 3>& filter_coefficients);

}  // namespace TextureConversionShaderGen
