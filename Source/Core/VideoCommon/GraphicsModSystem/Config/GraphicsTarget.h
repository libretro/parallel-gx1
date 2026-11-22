// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/XFMemory.h"

enum class GraphicsTargetKind
{
  DrawStartedTexture,
  LoadTexture,
  CreateTexture,
  EFB,
  XFB,
  Projection,
};

// What a mod's feature applies to; which fields are set depends on the kind
struct GraphicsTargetConfig
{
  GraphicsTargetKind kind = GraphicsTargetKind::DrawStartedTexture;
  // the texture targets, and a projection when has_texture_info is set
  bool has_texture_info = false;
  std::string texture_info;
  // EFB and XFB
  u32 width = 0;
  u32 height = 0;
  TextureFormat texture_format = TextureFormat::I4;
  // a projection
  ProjectionType projection_type = ProjectionType::Perspective;
};

/* A target field as a mod's file gives it: absent, a string, or a value of another kind. */
struct GraphicsTargetField
{
  enum class Kind
  {
    Absent,
    String,
    Other
  };
  Kind kind = Kind::Absent;
  std::string text;
};

// False, with the problem logged, for a target the mod gives wrongly
bool MakeTarget(const GraphicsTargetField& type, const GraphicsTargetField& texture_filename,
                const GraphicsTargetField& value, GraphicsTargetConfig* target);
