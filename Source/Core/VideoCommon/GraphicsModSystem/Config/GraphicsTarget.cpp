// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

#include <cstdlib>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/TextureCacheBase.h"

void FreeGraphicsTarget(GraphicsTargetConfig* target)
{
  std::free(target->texture_info);
  target->texture_info = nullptr;
}

void FreeGraphicsTargetField(GraphicsTargetField* field)
{
  std::free(field->text);
  field->text = nullptr;
  field->kind = GraphicsTargetField::Kind::Absent;
}

namespace
{
bool HasPrefix(const char* text, std::string_view prefix)
{
  return !std::strncmp(text, prefix.data(), prefix.size()) && std::strlen(text) >= prefix.size();
}

// The number in the size bytes at text, as TryParse reads a whole string
bool ParseU32(const char* text, size_t size, u32* out)
{
  char* const copy = static_cast<char*>(std::malloc(size + 1));
  if (!copy)
    return false;
  std::memcpy(copy, text, size);
  copy[size] = '\0';
  const bool parsed = TryParse(copy, out);
  std::free(copy);
  return parsed;
}

// How many pieces SplitString cuts the size bytes at text into at delim: none for nothing, and no
// empty piece after a delim at the end
size_t CountPieces(const char* text, size_t size, char delim)
{
  size_t i;
  size_t count;
  if (!size)
    return 0;
  count = 1;
  for (i = 0; i + 1 < size; i++)
  {
    if (text[i] == delim)
      count++;
  }
  return count;
}

const char* FindChar(const char* text, size_t size, char c)
{
  return static_cast<const char*>(std::memchr(text, c, size));
}

const char* FindLastChar(const char* text, size_t size, char c)
{
  while (size)
  {
    size--;
    if (text[size] == c)
      return text + size;
  }
  return nullptr;
}

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
  const char* const texture_filename = texture_filename_field.text;
  const size_t texture_filename_size = std::strlen(texture_filename);
  // the prefix and the character after it, whatever they are, are not looked at
  if (texture_filename_size < prefix.size() + 1)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, value in 'texture_filename' is not valid");
    return false;
  }
  const char* const name = texture_filename + prefix.size() + 1;
  const size_t name_size = texture_filename_size - prefix.size() - 1;
  if (CountPieces(name, name_size, '_') == 1)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, value in 'texture_filename' is not valid");
    return false;
  }
  if (CountPieces(name, name_size, 'x') != 2)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, width and height separator found more matches than expected");
    return false;
  }

  // The two pieces around the 'x', an 'x' at the very end dropped
  const char* const x = FindChar(name, name_size, 'x');
  const char* const width_piece = name;
  const size_t width_piece_size = static_cast<size_t>(x - name);
  const char* const height_piece = x + 1;
  size_t height_piece_size = name_size - width_piece_size - 1;
  if (FindChar(height_piece, height_piece_size, 'x'))
    height_piece_size--;

  const char* const width_underscore = FindLastChar(width_piece, width_piece_size, '_');
  const char* const width_str = width_underscore ? width_underscore + 1 : width_piece;
  const size_t width_size = width_piece_size - static_cast<size_t>(width_str - width_piece);
  if (!ParseU32(width_str, width_size, &fb->width))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, width not a number");
    return false;
  }

  const char* const height_underscore = FindChar(height_piece, height_piece_size, '_');
  if (!height_underscore || height_underscore == height_piece + height_piece_size - 1)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, underscore after height is missing or incomplete");
    return false;
  }
  if (!ParseU32(height_piece, static_cast<size_t>(height_underscore - height_piece), &fb->height))
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is "
                     "not valid, height not a number");
    return false;
  }

  const char* const format_str = height_underscore + 1;
  const size_t after_size = height_piece_size - static_cast<size_t>(format_str - height_piece);
  const char* const format_underscore = FindChar(format_str, after_size, '_');
  const size_t format_size =
      format_underscore ? static_cast<size_t>(format_underscore - format_str) : after_size;
  u32 format;
  if (!ParseU32(format_str, format_size, &format))
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

bool ExtractTextureFilename(const GraphicsTargetField& texture_filename, char** out)
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
  std::free(*out);
  *out = strdup(texture_filename.text);
  char* const texture_info = *out;
  if (!texture_info)
    return false;

  const char* type;
  if (HasPrefix(texture_info, EFB_DUMP_PREFIX))
    type = "an efb";
  else if (HasPrefix(texture_info, XFB_DUMP_PREFIX))
    type = "a xfb";
  else
    return true;

  // an EFB or XFB copy loses its count
  char* const letter_n = std::strstr(texture_info, "_n");
  if (!letter_n)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, value in 'texture_filename' "
              "is %s without a count",
              type);
    return false;
  }

  char* const post_underscore = std::strchr(letter_n + 2, '_');
  if (!post_underscore)
    *letter_n = '\0';
  else
    std::memmove(letter_n, post_underscore, std::strlen(post_underscore) + 1);
  return true;
}
}  // namespace

bool MakeTarget(const GraphicsTargetField& type_field, const GraphicsTargetField& texture_filename,
                const GraphicsTargetField& value, GraphicsTargetConfig* target)
{
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
  const char* const type = type_field.text;
  if (!std::strcmp(type, "draw_started") || !std::strcmp(type, "load_texture") ||
      !std::strcmp(type, "create_texture"))
  {
    target->kind = !std::strcmp(type, "draw_started") ? GraphicsTargetKind::DrawStartedTexture :
                   !std::strcmp(type, "load_texture") ? GraphicsTargetKind::LoadTexture :
                                                        GraphicsTargetKind::CreateTexture;
    target->has_texture_info = true;
    return ExtractTextureFilename(texture_filename, &target->texture_info);
  }
  else if (!std::strcmp(type, "efb"))
  {
    target->kind = GraphicsTargetKind::EFB;
    return MakeFBTarget(texture_filename, EFB_DUMP_PREFIX, target);
  }
  else if (!std::strcmp(type, "xfb"))
  {
    target->kind = GraphicsTargetKind::XFB;
    return MakeFBTarget(texture_filename, EFB_DUMP_PREFIX, target);
  }
  else if (!std::strcmp(type, "projection"))
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
    if (!std::strcmp(value.text, "2d"))
    {
      target->projection_type = ProjectionType::Orthographic;
    }
    else if (!std::strcmp(value.text, "3d"))
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
