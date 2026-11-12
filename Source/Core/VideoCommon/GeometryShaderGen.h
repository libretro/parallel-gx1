// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;

#pragma pack(1)
struct geometry_shader_uid_data
{
  u32 NumValues() const { return sizeof(geometry_shader_uid_data); }
  bool IsPassthrough() const;

  u32 numTexGens : 4;
  u32 primitive_type : 2;
};
#pragma pack()

using GeometryShaderUid = ShaderUid<geometry_shader_uid_data>;

/* What BuildGeometryShader builds the stage for. */
struct GeometryShaderBuildArgs
{
  const ShaderHostConfig* host_config;
  const geometry_shader_uid_data* uid_data;
  APIType api_type;
};
/* The stage built by calls: for AbstractGfx::CreateShaderFromBuild. */
bool BuildGeometryShader(struct shader_build* sb, const void* arg);
GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type);
/* Every geometry shader UID, into uids: returns how many. */
enum
{
  GEOMETRY_SHADER_UIDS = 27
};
u32 EnumerateGeometryShaderUids(GeometryShaderUid* uids);
