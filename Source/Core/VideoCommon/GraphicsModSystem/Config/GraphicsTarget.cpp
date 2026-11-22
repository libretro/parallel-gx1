// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/TextureCacheBase.h"

namespace
{
bool MakeFBTarget(const GraphicsTargetField& texture_filename_field, std::string_view prefix,
                  GraphicsTargetConfig* fb)
{
  if (texture_filename_field.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'texture_filename' not found");
    return false;
  }
  if (texture_filename_field.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG(
        VIDEO,
        "Failed to load mod configuration file, option 'texture_filename' is not a string type");
    return false;
  }
  const std::string& texture_filename = texture_filename_field.text;
  const auto texture_filename_without_prefix = texture_filename.substr(prefix.size() + 1);
  const auto split_str_values = SplitString(texture_filename_without_prefix, '_');
  if (split_str_values.size() == 1)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, value in 'texture_filename' is not valid");
    return false;
  }
  const auto split_width_height_values = SplitString(texture_filename_without_prefix, 'x');
  if (split_width_height_values.size() != 2)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, width and height separator found more matches than expected");
    return false;
  }

  const std::size_t width_underscore_pos = split_width_height_values[0].find_last_of('_');
  std::string width_str;
  if (width_underscore_pos == std::string::npos)
  {
    width_str = split_width_height_values[0];
  }
  else
  {
    width_str = split_width_height_values[0].substr(width_underscore_pos + 1);
  }
  if (!TryParse(width_str, &fb->width))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, width not a number");
    return false;
  }

  const std::size_t height_underscore_pos = split_width_height_values[1].find_first_of('_');
  if (height_underscore_pos == std::string::npos ||
      height_underscore_pos == split_width_height_values[1].size() - 1)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, underscore after height is missing or incomplete");
    return false;
  }
  const std::string height_str = split_width_height_values[1].substr(0, height_underscore_pos);
  if (!TryParse(height_str, &fb->height))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, height not a number");
    return false;
  }

  const std::size_t format_underscore_pos =
      split_width_height_values[1].find_first_of('_', height_underscore_pos + 1);

  std::string format_str;
  if (format_underscore_pos == std::string::npos)
  {
    format_str = split_width_height_values[1].substr(height_underscore_pos + 1);
  }
  else
  {
    format_str = split_width_height_values[1].substr(
        height_underscore_pos + 1, (format_underscore_pos - height_underscore_pos) - 1);
  }
  u32 format;
  if (!TryParse(format_str, &format))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, texture format is not a number");
    return false;
  }
  if (!IsValidTextureFormat(static_cast<TextureFormat>(format)))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, texture format is not valid");
    return false;
  }
  fb->texture_format = static_cast<TextureFormat>(format);

  return true;
}

bool ExtractTextureFilename(const GraphicsTargetField& texture_filename, std::string* out)
{
  if (texture_filename.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'texture_filename' not found");
    return false;
  }
  if (texture_filename.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG(
        VIDEO,
        "Failed to load mod configuration file, option 'texture_filename' is not a string type");
    return false;
  }
  std::string& texture_info = *out;
  texture_info = texture_filename.text;

  const char* type;
  if (texture_info.starts_with(EFB_DUMP_PREFIX))
    type = "an efb";
  else if (texture_info.starts_with(XFB_DUMP_PREFIX))
    type = "a xfb";
  else
    return true;

  // an EFB or XFB copy loses its count
  const auto letter_n_pos = texture_info.find("_n");
  if (letter_n_pos == std::string::npos)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, value in 'texture_filename' "
              "is %s without a count",
              type);
    return false;
  }

  const auto post_underscore = texture_info.find_first_of('_', letter_n_pos + 2);
  if (post_underscore == std::string::npos)
    texture_info.erase(letter_n_pos, texture_info.size() - letter_n_pos);
  else
    texture_info.erase(letter_n_pos, post_underscore - letter_n_pos);
  return true;
}
}  // namespace

bool MakeTarget(const GraphicsTargetField& type_field, const GraphicsTargetField& texture_filename,
                const GraphicsTargetField& value, GraphicsTargetConfig* target)
{
  *target = GraphicsTargetConfig{};
  if (type_field.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'type' not found");
    return false;
  }
  if (type_field.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'type' is not a string type");
    return false;
  }
  const std::string& type = type_field.text;
  if (type == "draw_started" || type == "load_texture" || type == "create_texture")
  {
    target->kind = type == "draw_started" ? GraphicsTargetKind::DrawStartedTexture :
                   type == "load_texture" ? GraphicsTargetKind::LoadTexture :
                                            GraphicsTargetKind::CreateTexture;
    target->has_texture_info = true;
    return ExtractTextureFilename(texture_filename, &target->texture_info);
  }
  else if (type == "efb")
  {
    target->kind = GraphicsTargetKind::EFB;
    return MakeFBTarget(texture_filename, EFB_DUMP_PREFIX, target);
  }
  else if (type == "xfb")
  {
    target->kind = GraphicsTargetKind::XFB;
    return MakeFBTarget(texture_filename, EFB_DUMP_PREFIX, target);
  }
  else if (type == "projection")
  {
    target->kind = GraphicsTargetKind::Projection;
    if (texture_filename.kind != GraphicsTargetField::Kind::Absent)
    {
      if (!ExtractTextureFilename(texture_filename, &target->texture_info))
        return false;
      target->has_texture_info = true;
    }
    if (value.kind == GraphicsTargetField::Kind::Absent)
    {
      ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'value' not found");
      return false;
    }
    if (value.kind != GraphicsTargetField::Kind::String)
    {
      ERROR_LOG(VIDEO,
                "Failed to load mod configuration file, option 'value' is not a string type");
      return false;
    }
    const std::string& value_str = value.text;
    if (value_str == "2d")
    {
      target->projection_type = ProjectionType::Orthographic;
    }
    else if (value_str == "3d")
    {
      target->projection_type = ProjectionType::Perspective;
    }
    else
    {
      ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'value' is not a valid "
                       "value, valid values are: 2d, 3d");
      return false;
    }
    return true;
  }
  else
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, option 'type' is not a valid value");
  }
  return false;
}
