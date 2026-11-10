// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/DynamicInputTextures/DITSpecification.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include <formats/rjson.h>
#include <streams/file_stream.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"

namespace InputCommon::DynamicInputTextures
{
namespace
{
using HostDevices = std::unordered_map<std::string, Data::HostKeyToImagePath>;

/* A texture as the file gives it, before the file's defaults are known. */
struct PendingTexture
{
  Data data;
  HostDevices host_controls;
  bool has_image = false;
  bool has_emulated_controls = false;
  bool has_host_controls = false;
};

struct Reader
{
  rjson_t* json;
  const std::string* json_file;
};

int ReadFile(void* buf, int len, void* user)
{
  return static_cast<int>(filestream_read(static_cast<RFILE*>(user), buf, len));
}

/* Past the value whose first token was t; false where the document is broken. */
bool Skip(rjson_t* json, enum rjson_type t)
{
  unsigned depth = 0;
  for (;;)
  {
    if (t == RJSON_OBJECT || t == RJSON_ARRAY)
      depth++;
    else if (t == RJSON_OBJECT_END || t == RJSON_ARRAY_END)
      depth--;
    else if (t == RJSON_ERROR || t == RJSON_DONE)
      return false;
    if (!depth)
      return true;
    t = rjson_next(json);
  }
}

std::string_view Text(rjson_t* json)
{
  size_t len;
  const char* s = rjson_get_string(json, &len);
  return std::string_view(s, len);
}

/* A value as a string, as the file's host controls take any: a string's text, a number's
 * digits, true, false or null, and the kind of an array or object. */
bool ValueString(rjson_t* json, enum rjson_type t, std::string* out)
{
  switch (t)
  {
  case RJSON_STRING:
  case RJSON_NUMBER:
    *out = Text(json);
    return true;
  case RJSON_TRUE:
    *out = "true";
    return true;
  case RJSON_FALSE:
    *out = "false";
    return true;
  case RJSON_NULL:
    *out = "null";
    return true;
  case RJSON_ARRAY:
    *out = "array";
    return Skip(json, t);
  case RJSON_OBJECT:
    *out = "object";
    return Skip(json, t);
  default:
    return false;
  }
}

/* { "device": { "control": "image", ... }, ... } after its '{' */
bool ReadHostControls(Reader* r, HostDevices* out)
{
  enum rjson_type t;
  while ((t = rjson_next(r->json)) == RJSON_STRING)
  {
    const std::string host_device(Text(r->json));
    t = rjson_next(r->json);
    if (t != RJSON_OBJECT)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because 'host_controls' "
                    "map key '{}' is incorrect type ",
                    *r->json_file, host_device);
      return false;
    }
    auto& host_control_to_imagename = (*out)[host_device];
    while ((t = rjson_next(r->json)) == RJSON_STRING)
    {
      const std::string host_control(Text(r->json));
      std::string image_name;
      if (!ValueString(r->json, rjson_next(r->json), &image_name))
        return false;
      host_control_to_imagename.insert_or_assign(host_control, std::move(image_name));
    }
    if (t != RJSON_OBJECT_END)
      return false;
  }
  return t == RJSON_OBJECT_END;
}

/* [left, top, right, bottom] after its '[' */
bool ReadRegion(Reader* r, const std::string& controller, const std::string& control, Rect* rect)
{
  u32 v[4];
  u32 n = 0;
  bool numbers = true;
  enum rjson_type t;
  while ((t = rjson_next(r->json)) != RJSON_ARRAY_END)
  {
    if (t == RJSON_NUMBER)
    {
      if (n < 4)
        v[n] = static_cast<u32>(rjson_get_double(r->json));
    }
    else
    {
      numbers = false;
      if (!Skip(r->json, t))
        return false;
    }
    n++;
  }
  if (n != 4)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because emulated controller '{}' "
                  "key '{}' has a region that does not have 4 offsets (left, top, right, "
                  "bottom).",
                  *r->json_file, controller, control);
    return false;
  }
  if (!numbers)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because emulated controller '{}' "
                  "key '{}' has a region that has the incorrect offset type.",
                  *r->json_file, controller, control);
    return false;
  }
  rect->left = v[0];
  rect->top = v[1];
  rect->right = v[2];
  rect->bottom = v[3];
  return true;
}

/* "emulated_controls": { "controller": { "control": [[l, t, r, b], ...], ... }, ... } */
bool ReadEmulatedControls(Reader* r, Data* data)
{
  enum rjson_type t;
  while ((t = rjson_next(r->json)) == RJSON_STRING)
  {
    const std::string controller(Text(r->json));
    if (rjson_next(r->json) != RJSON_OBJECT)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because 'emulated_controls' "
                    "map key '{}' is incorrect type.  Expected map  ",
                    *r->json_file, controller);
      return false;
    }
    auto& key_to_regions = data->m_emulated_controllers[controller];
    while ((t = rjson_next(r->json)) == RJSON_STRING)
    {
      const std::string control(Text(r->json));
      std::vector<Rect> region_rects;
      if (rjson_next(r->json) != RJSON_ARRAY)
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load dynamic input json file '{}' because emulated controller "
                      "'{}' key '{}' has incorrect value type.  Expected array ",
                      *r->json_file, controller, control);
        return false;
      }
      while ((t = rjson_next(r->json)) != RJSON_ARRAY_END)
      {
        Rect rect;
        if (t != RJSON_ARRAY)
        {
          ERROR_LOG_FMT(VIDEO,
                        "Failed to load dynamic input json file '{}' because emulated controller "
                        "'{}' key '{}' has a region with the incorrect type.  Expected array ",
                        *r->json_file, controller, control);
          return false;
        }
        if (!ReadRegion(r, controller, control, &rect))
          return false;
        region_rects.push_back(rect);
      }
      key_to_regions.insert_or_assign(control, std::move(region_rects));
    }
    if (t != RJSON_OBJECT_END)
      return false;
  }
  return t == RJSON_OBJECT_END;
}

/* one of "output_textures": { "name": { ... } } */
bool ReadTexture(Reader* r, PendingTexture* texture)
{
  enum rjson_type t;
  while ((t = rjson_next(r->json)) == RJSON_STRING)
  {
    const std::string_view key = Text(r->json);
    const bool image = key == "image";
    const bool emulated = key == "emulated_controls";
    const bool host = key == "host_controls";
    t = rjson_next(r->json);
    if (image && t == RJSON_STRING)
    {
      texture->data.m_image_name = Text(r->json);
      texture->has_image = true;
    }
    else if (emulated && t == RJSON_OBJECT)
    {
      texture->data.m_emulated_controllers.clear();
      if (!ReadEmulatedControls(r, &texture->data))
        return false;
      texture->has_emulated_controls = true;
    }
    else if (host && t == RJSON_OBJECT)
    {
      texture->host_controls.clear();
      if (!ReadHostControls(r, &texture->host_controls))
        return false;
      texture->has_host_controls = true;
    }
    else
    {
      if (image)
        texture->has_image = false;
      else if (emulated)
        texture->has_emulated_controls = false;
      else if (host)
        texture->has_host_controls = false;
      if (!Skip(r->json, t))
        return false;
    }
  }
  return t == RJSON_OBJECT_END;
}
}  // namespace

bool LoadSpecification(const std::string& json_file, std::vector<Data>& input_textures,
                       const std::string& base_path)
{
  RFILE* const file = filestream_open(json_file.c_str(), RETRO_VFS_FILE_ACCESS_READ,
                                      RETRO_VFS_FILE_ACCESS_HINT_NONE);
  std::vector<std::pair<std::string, PendingTexture>> textures;
  HostDevices default_host_controls;
  std::string generated_folder_name =
      fmt::format("{}_Generated", SConfig::GetInstance().GetGameID());
  double specification = 1;
  bool preserve_aspect_ratio = true;
  bool has_output_textures = false;
  bool ok = false;
  Reader r;

  if (!file)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load dynamic input json file '{}': cannot read it", json_file);
    return false;
  }
  r.json = rjson_open_user(ReadFile, file, 4096);
  r.json_file = &json_file;
  if (r.json && rjson_next(r.json) == RJSON_OBJECT)
  {
    enum rjson_type t;
    ok = true;
    while (ok && (t = rjson_next(r.json)) == RJSON_STRING)
    {
      const std::string_view key = Text(r.json);
      const bool output = key == "output_textures";
      const bool defaults = key == "default_host_controls";
      const bool spec = key == "specification";
      const bool aspect = key == "preserve_aspect_ratio";
      const bool folder = key == "generated_folder_name";
      t = rjson_next(r.json);
      if (output)
        has_output_textures = false;
      if (spec && t == RJSON_NUMBER)
        specification = rjson_get_double(r.json);
      else if (aspect && (t == RJSON_TRUE || t == RJSON_FALSE))
        preserve_aspect_ratio = t == RJSON_TRUE;
      else if (folder && t == RJSON_STRING)
        generated_folder_name = Text(r.json);
      else if (defaults && t == RJSON_OBJECT)
      {
        default_host_controls.clear();
        ok = ReadHostControls(&r, &default_host_controls);
      }
      else if (output && t == RJSON_OBJECT)
      {
        has_output_textures = true;
        textures.clear();
        while (ok && (t = rjson_next(r.json)) == RJSON_STRING)
        {
          std::pair<std::string, PendingTexture> texture;
          texture.first = Text(r.json);
          t = rjson_next(r.json);
          if (t != RJSON_OBJECT)
            ok = Skip(r.json, t);
          else
            ok = ReadTexture(&r, &texture.second);
          textures.emplace_back(std::move(texture));
        }
        ok = ok && t == RJSON_OBJECT_END;
      }
      else
        ok = Skip(r.json, t);
    }
  }
  if (r.json && rjson_get_error(r.json)[0])
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load dynamic input json file '{}' due to parse error: {}",
                  json_file, rjson_get_error(r.json));
    ok = false;
  }
  if (r.json)
    rjson_free(r.json);
  filestream_close(file);
  if (!ok)
    return false;

  if (specification < static_cast<double>(std::numeric_limits<u8>::min()) ||
      specification > static_cast<double>(std::numeric_limits<u8>::max()))
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load dynamic input json file '{}', specification '{}' is not within bounds",
        json_file, specification);
    return false;
  }
  if (static_cast<u8>(specification) != 1)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}', specification '{}' is invalid",
                  json_file, static_cast<u8>(specification));
    return false;
  }
  if (!has_output_textures)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load dynamic input json file '{}' because 'output_textures' is missing or "
        "was not of type object",
        json_file);
    return false;
  }

  /* in the order of their names, as the textures were taken before */
  std::stable_sort(textures.begin(), textures.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [name, texture] : textures)
  {
    Data& texture_data = texture.data;
    texture_data.m_hires_texture_name = name;
    if (!texture.has_image || !texture.has_emulated_controls)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because required fields "
                    "'image', or 'emulated_controls' are either "
                    "missing or the incorrect type",
                    json_file);
      return false;
    }
    texture_data.m_preserve_aspect_ratio = preserve_aspect_ratio;
    texture_data.m_generated_folder_name = generated_folder_name;

    const std::string image_full_path = base_path + texture_data.m_image_name;
    if (!File::Exists(image_full_path))
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because the image '{}' "
                    "could not be loaded",
                    json_file, image_full_path);
      return false;
    }

    // Default to the default controls but overwrite if the creator
    // has provided something specific
    texture_data.m_host_devices =
        texture.has_host_controls ? std::move(texture.host_controls) : default_host_controls;
    if (texture_data.m_host_devices.empty())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because field "
                    "'host_controls' is missing ",
                    json_file);
      return false;
    }
    input_textures.emplace_back(std::move(texture_data));
  }
  return true;
}
}  // namespace InputCommon::DynamicInputTextures
