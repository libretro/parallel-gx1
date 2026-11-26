// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

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

// What a mod's feature applies to; which fields are set depends on the kind. Zeroed is empty, and
// FreeGraphicsTarget frees what it owns.
struct GraphicsTargetConfig
{
  GraphicsTargetKind kind;
  // the texture targets, and a projection when has_texture_info is set
  bool has_texture_info;
  char* texture_info;  // owned, null when there is none
  // EFB and XFB
  u32 width;
  u32 height;
  TextureFormat texture_format;
  // a projection
  ProjectionType projection_type;
};

void FreeGraphicsTarget(GraphicsTargetConfig* target);

/* A target field as a mod's file gives it: absent, a string, or a value of another kind. Zeroed is
 * absent; FreeGraphicsTargetField frees its text. */
struct GraphicsTargetField
{
  enum class Kind
  {
    Absent,
    String,
    Other
  };
  Kind kind;
  char* text;  // owned, the string's
};

void FreeGraphicsTargetField(GraphicsTargetField* field);

// False, with the problem logged, for a target the mod gives wrongly; target is to be zeroed first,
// and is freed with FreeGraphicsTarget either way.
bool MakeTarget(const GraphicsTargetField& type, const GraphicsTargetField& texture_filename,
                const GraphicsTargetField& value, GraphicsTargetConfig* target);
