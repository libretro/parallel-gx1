// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <variant>

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/XFMemory.h"

struct TextureTarget
{
  std::string m_texture_info_string;
};

struct DrawStartedTextureTarget final : public TextureTarget
{
};

struct LoadTextureTarget final : public TextureTarget
{
};

struct CreateTextureTarget final : public TextureTarget
{
};

struct FBTarget
{
  u32 m_height = 0;
  u32 m_width = 0;
  TextureFormat m_texture_format = TextureFormat::I4;
};

struct EFBTarget final : public FBTarget
{
};

struct XFBTarget final : public FBTarget
{
};

struct ProjectionTarget
{
  std::optional<std::string> m_texture_info_string;
  ProjectionType m_projection_type = ProjectionType::Perspective;
};

using GraphicsTargetConfig =
    std::variant<DrawStartedTextureTarget, LoadTextureTarget, CreateTextureTarget, EFBTarget,
                 XFBTarget, ProjectionTarget>;

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

std::optional<GraphicsTargetConfig> MakeTarget(const GraphicsTargetField& type,
                                               const GraphicsTargetField& texture_filename,
                                               const GraphicsTargetField& value);
