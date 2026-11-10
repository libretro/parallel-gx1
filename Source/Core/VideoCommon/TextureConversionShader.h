// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <utility>

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;
enum class TextureFormat;
enum class EFBCopyFormat;
enum class TLUTFormat;
enum TexelBufferFormat : u32;
struct EFBCopyParams;

namespace TextureConversionShaderTiled
{
u16 GetEncodedSampleCount(EFBCopyFormat format);

u32 GetEncodingFlags(const EFBCopyParams& params);
/* What BuildEncodingShader builds the encoder of an EFB copy to RAM for: the rest of the copy is
 * read from the copy_flags uniform, as GetEncodingFlags packs it. */
struct EncodingShaderBuildArgs
{
  EFBCopyFormat format;
  APIType api_type;
};
/* The stage built by calls: for AbstractGfx::CreateShaderFromBuild. */
bool BuildEncodingShader(struct shader_build* sb, const void* arg);

// Information required to compile and dispatch a texture decoding shader.
struct DecodingShaderInfo
{
  TexelBufferFormat buffer_format;
  u32 palette_size;
  u32 group_size_x;
  u32 group_size_y;
  bool group_flatten;
  const char* shader_body;
};

// Obtain shader information for the specified texture format.
// If this format does not have a shader written for it, returns nullptr.
const DecodingShaderInfo* GetDecodingShaderInfo(TextureFormat format);

// Determine how many thread groups should be dispatched for an image of the specified width/height.
// First is the number of X groups, second is the number of Y groups, Z is always one.
std::pair<u32, u32> GetDispatchCount(const DecodingShaderInfo* info, u32 width, u32 height);

// Returns the GLSL string containing the texture decoding shader for the specified format.
ShaderCode GenerateDecodingShader(TextureFormat format, std::optional<TLUTFormat> palette_format,
                                   APIType api_type);

// Returns the GLSL string containing the palette conversion shader for the specified format.
ShaderCode GeneratePaletteConversionShader(TLUTFormat palette_format, APIType api_type);

}  // namespace TextureConversionShaderTiled
