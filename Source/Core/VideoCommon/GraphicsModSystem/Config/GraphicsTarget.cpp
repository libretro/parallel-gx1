// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/VariantUtil.h"
#include "VideoCommon/TextureCacheBase.h"

namespace
{
template <typename T, std::enable_if_t<std::is_base_of_v<FBTarget, T>, int> = 0>
std::optional<T> MakeFBTarget(const GraphicsTargetField& texture_filename_field,
                              std::string_view prefix)
{
  T fb;
  if (texture_filename_field.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'texture_filename' not found");
    return std::nullopt;
  }
  if (texture_filename_field.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load mod configuration file, option 'texture_filename' is not a string type");
    return std::nullopt;
  }
  const std::string& texture_filename = texture_filename_field.text;
  const auto texture_filename_without_prefix = texture_filename.substr(prefix.size() + 1);
  const auto split_str_values = SplitString(texture_filename_without_prefix, '_');
  if (split_str_values.size() == 1)
  {
    ERROR_LOG_FMT(
        VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is not valid");
    return std::nullopt;
  }
  const auto split_width_height_values = SplitString(texture_filename_without_prefix, 'x');
  if (split_width_height_values.size() != 2)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, width and height separator found more matches than expected");
    return std::nullopt;
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
  if (!TryParse(width_str, &fb.m_width))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, width not a number");
    return std::nullopt;
  }

  const std::size_t height_underscore_pos = split_width_height_values[1].find_first_of('_');
  if (height_underscore_pos == std::string::npos ||
      height_underscore_pos == split_width_height_values[1].size() - 1)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, underscore after height is missing or incomplete");
    return std::nullopt;
  }
  const std::string height_str = split_width_height_values[1].substr(0, height_underscore_pos);
  if (!TryParse(height_str, &fb.m_height))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, height not a number");
    return std::nullopt;
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
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, texture format is not a number");
    return std::nullopt;
  }
  if (!IsValidTextureFormat(static_cast<TextureFormat>(format)))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                         "not valid, texture format is not valid");
    return std::nullopt;
  }
  fb.m_texture_format = static_cast<TextureFormat>(format);

  return fb;
}
std::optional<std::string> ExtractTextureFilename(const GraphicsTargetField& texture_filename)
{
  if (texture_filename.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'texture_filename' not found");
    return std::nullopt;
  }
  if (texture_filename.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load mod configuration file, option 'texture_filename' is not a string type");
    return std::nullopt;
  }
  std::string texture_info = texture_filename.text;

  const auto handle_fb_texture =
      [&texture_info](std::string_view type) -> std::optional<std::string> {
    const auto letter_n_pos = texture_info.find("_n");
    if (letter_n_pos == std::string::npos)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load mod configuration file, value in 'texture_filename' "
                    "is {} without a count",
                    type);
      return std::nullopt;
    }

    const auto post_underscore = texture_info.find_first_of('_', letter_n_pos + 2);
    if (post_underscore == std::string::npos)
      return texture_info.erase(letter_n_pos, texture_info.size() - letter_n_pos);
    else
      return texture_info.erase(letter_n_pos, post_underscore - letter_n_pos);
  };

  if (texture_info.starts_with(EFB_DUMP_PREFIX))
    return handle_fb_texture("an efb");
  else if (texture_info.starts_with(XFB_DUMP_PREFIX))
    return handle_fb_texture("a xfb");
  return texture_info;
}
}  // namespace

std::optional<GraphicsTargetConfig> MakeTarget(const GraphicsTargetField& type_field,
                                               const GraphicsTargetField& texture_filename,
                                               const GraphicsTargetField& value)
{
  if (type_field.kind == GraphicsTargetField::Kind::Absent)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'type' not found");
    return std::nullopt;
  }
  if (type_field.kind != GraphicsTargetField::Kind::String)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'type' is not a string type");
    return std::nullopt;
  }
  const std::string& type = type_field.text;
  if (type == "draw_started")
  {
    std::optional<std::string> texture_info = ExtractTextureFilename(texture_filename);
    if (!texture_info.has_value())
      return std::nullopt;

    DrawStartedTextureTarget target;
    target.m_texture_info_string = texture_info.value();
    return target;
  }
  else if (type == "load_texture")
  {
    std::optional<std::string> texture_info = ExtractTextureFilename(texture_filename);
    if (!texture_info.has_value())
      return std::nullopt;

    LoadTextureTarget target;
    target.m_texture_info_string = texture_info.value();
    return target;
  }
  else if (type == "create_texture")
  {
    std::optional<std::string> texture_info = ExtractTextureFilename(texture_filename);
    if (!texture_info.has_value())
      return std::nullopt;

    CreateTextureTarget target;
    target.m_texture_info_string = texture_info.value();
    return target;
  }
  else if (type == "efb")
  {
    return MakeFBTarget<EFBTarget>(texture_filename, EFB_DUMP_PREFIX);
  }
  else if (type == "xfb")
  {
    return MakeFBTarget<XFBTarget>(texture_filename, EFB_DUMP_PREFIX);
  }
  else if (type == "projection")
  {
    ProjectionTarget target;
    if (texture_filename.kind != GraphicsTargetField::Kind::Absent)
    {
      std::optional<std::string> texture_info = ExtractTextureFilename(texture_filename);
      if (!texture_info.has_value())
        return std::nullopt;
      target.m_texture_info_string = texture_info;
    }
    if (value.kind == GraphicsTargetField::Kind::Absent)
    {
      ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'value' not found");
      return std::nullopt;
    }
    if (value.kind != GraphicsTargetField::Kind::String)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load mod configuration file, option 'value' is not a string type");
      return std::nullopt;
    }
    const std::string& value_str = value.text;
    if (value_str == "2d")
    {
      target.m_projection_type = ProjectionType::Orthographic;
    }
    else if (value_str == "3d")
    {
      target.m_projection_type = ProjectionType::Perspective;
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'value' is not a valid "
                           "value, valid values are: 2d, 3d");
      return std::nullopt;
    }
    return target;
  }
  else
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'type' is not a valid value");
  }
  return std::nullopt;
}
