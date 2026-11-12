// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/PixelShaderGen.h"

namespace UberShader
{
#pragma pack(1)
struct vertex_ubershader_uid_data
{
  u32 num_texgens : 4;

  u32 NumValues() const { return sizeof(vertex_ubershader_uid_data); }
};
#pragma pack()

using VertexShaderUid = ShaderUid<vertex_ubershader_uid_data>;

VertexShaderUid GetVertexShaderUid();

/* What BuildVertexShader builds the stage for. */
struct VertexShaderBuildArgs
{
  const ShaderHostConfig* host_config;
  const vertex_ubershader_uid_data* uid_data;
  APIType api_type;
};
/* The stage built by calls: for AbstractGfx::CreateShaderFromBuild. */
bool BuildVertexShader(struct shader_build* sb, const void* arg);
/* Every vertex ubershader UID, into uids: returns how many. */
enum
{
  VERTEX_UBERSHADER_UIDS = 9
};
u32 EnumerateVertexShaderUids(VertexShaderUid* uids);
}  // namespace UberShader
