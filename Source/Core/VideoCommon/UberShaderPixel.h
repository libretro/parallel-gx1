// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;

namespace UberShader
{
#pragma pack(1)
struct pixel_ubershader_uid_data
{
  u32 num_texgens : 4;
  u32 early_depth : 1;
  u32 per_pixel_depth : 1;
  u32 uint_output : 1;
  u32 no_dual_src : 1;

  u32 NumValues() const { return sizeof(pixel_ubershader_uid_data); }
};
#pragma pack()

using PixelShaderUid = ShaderUid<pixel_ubershader_uid_data>;

PixelShaderUid GetPixelShaderUid();

/* What BuildPixelShader builds the stage for. */
struct PixelShaderBuildArgs
{
  const ShaderHostConfig* host_config;
  const pixel_ubershader_uid_data* uid_data;
  APIType api_type;
};
/* The stage built by calls: for AbstractGfx::CreateShaderFromBuild. */
bool BuildPixelShader(struct shader_build* sb, const void* arg);

/* Every pixel ubershader UID, into uids: returns how many. */
enum
{
  PIXEL_UBERSHADER_UIDS = 108
};
u32 EnumeratePixelShaderUids(PixelShaderUid* uids);
void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid);
}  // namespace UberShader
