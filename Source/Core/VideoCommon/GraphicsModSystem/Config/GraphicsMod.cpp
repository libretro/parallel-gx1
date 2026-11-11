// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"

#include <cinttypes>
#include <cstring>
#include <string_view>

#include <cstdio>

#include <formats/rjson.h>
#include <streams/file_stream.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Constants.h"

namespace
{
int ReadFile(void* buf, int len, void* user)
{
  return static_cast<int>(filestream_read(static_cast<RFILE*>(user), buf, len));
}

/* A json file read as it is parsed. */
struct Document
{
  explicit Document(const std::string& path)
      : file(filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ,
                             RETRO_VFS_FILE_ACCESS_HINT_NONE)),
        json(file ? rjson_open_user(ReadFile, file, 4096) : nullptr)
  {
  }
  ~Document()
  {
    if (json)
      rjson_free(json);
    if (file)
      filestream_close(file);
  }
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  RFILE* file;
  rjson_t* json;
};

void LogParseError(rjson_t* json, std::string_view what, const std::string& path)
{
  ERROR_LOG(VIDEO,
            "Failed to load %.*s json file '%s' due to parse error: %s at line %" PRIu64
            ":%" PRIu64,
            static_cast<int>(what.size()), what.data(), path.c_str(), rjson_get_error(json),
            (u64)rjson_get_source_line(json), (u64)rjson_get_source_column(json));
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

/* The current member name's index in names, or -1. */
int Key(rjson_t* json, const char* const* names, int n)
{
  const std::string_view key = Text(json);
  for (int k = 0; k < n; k++)
  {
    if (key == names[k])
      return k;
  }
  return -1;
}

bool ReadField(rjson_t* json, enum rjson_type t, GraphicsTargetField* field)
{
  if (t == RJSON_STRING)
  {
    field->kind = GraphicsTargetField::Kind::String;
    field->text = Text(json);
    return true;
  }
  field->kind = GraphicsTargetField::Kind::Other;
  field->text.clear();
  return Skip(json, t);
}

/* A string member into out, or false with what it was logged. */
bool ReadString(rjson_t* json, enum rjson_type t, std::string* out, std::string_view what)
{
  if (t == RJSON_STRING)
  {
    *out = Text(json);
    return true;
  }
  ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified %.*s is not a string",
            static_cast<int>(what.size()), what.data());
  Skip(json, t);
  return false;
}

/* The elements of an array after its '[', each through read. */
template <typename F>
bool ReadArray(rjson_t* json, F read)
{
  enum rjson_type t;
  while ((t = rjson_next(json)) != RJSON_ARRAY_END)
  {
    if (!read(t))
      return false;
  }
  return true;
}

/* { "title": ..., "author": ..., "description": ... } after its '{' */
bool ReadMeta(rjson_t* json, GraphicsModConfig* mod)
{
  static const char* const names[] = {"title", "author", "description"};
  std::string* const fields[] = {&mod->m_title, &mod->m_author, &mod->m_description};
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    t = rjson_next(json);
    if (k >= 0 && t == RJSON_STRING)
      *fields[k] = Text(json);
    else if (!Skip(json, t))
      return false;
  }
  return t == RJSON_OBJECT_END;
}

/* { "type": ..., "texture_filename": ..., "value": ... } after its '{' */
bool ReadTarget(rjson_t* json, std::vector<GraphicsTargetConfig>* targets)
{
  static const char* const names[] = {"type", "texture_filename", "value"};
  GraphicsTargetField fields[3];
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    t = rjson_next(json);
    if (!(k >= 0 ? ReadField(json, t, &fields[k]) : Skip(json, t)))
      return false;
  }
  if (t != RJSON_OBJECT_END)
    return false;
  auto target = MakeTarget(fields[0], fields[1], fields[2]);
  if (!target)
    return false;
  targets->push_back(std::move(*target));
  return true;
}

/* { "name": ..., "targets": [...] } after its '{' */
bool ReadGroup(rjson_t* json, GraphicsTargetGroupConfig* group)
{
  static const char* const names[] = {"name", "targets"};
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 2);
    t = rjson_next(json);
    if (k == 0)
    {
      if (!ReadString(json, t, &group->m_name, "group's name"))
        return false;
    }
    else if (k == 1)
    {
      if (t != RJSON_ARRAY)
      {
        ERROR_LOG(
            VIDEO,
            "Failed to load mod configuration file, specified group's targets is not an array");
        return false;
      }
      group->m_targets.clear();
      if (!ReadArray(json, [&](enum rjson_type e) {
            if (e != RJSON_OBJECT)
            {
              ERROR_LOG(VIDEO, "Failed to load shader configuration file, specified target "
                               "is not a json object");
              return false;
            }
            return ReadTarget(json, &group->m_targets);
          }))
      {
        return false;
      }
    }
    else if (!Skip(json, t))
    {
      return false;
    }
  }
  return t == RJSON_OBJECT_END;
}

/* { "X": ..., "Y": ..., "Z": ... } after its '{'; members that are not numbers are 0 */
bool ReadActionData(rjson_t* json, Common::Vec3* xyz)
{
  static const char* const names[] = {"X", "Y", "Z"};
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    t = rjson_next(json);
    if (k >= 0)
      xyz->data[k] = t == RJSON_NUMBER ? static_cast<float>(rjson_get_double(json)) : 0.0f;
    if (!Skip(json, t))
      return false;
  }
  return t == RJSON_OBJECT_END;
}

/* { "group": ..., "action": ..., "action_data": ... } after its '{' */
bool ReadFeature(rjson_t* json, GraphicsModFeatureConfig* feature)
{
  static const char* const names[] = {"group", "action", "action_data"};
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    t = rjson_next(json);
    if (k == 0)
    {
      if (!ReadString(json, t, &feature->m_group, "feature's group"))
        return false;
    }
    else if (k == 1)
    {
      if (!ReadString(json, t, &feature->m_action, "feature's action"))
        return false;
    }
    else if (k == 2)
    {
      feature->m_action_xyz = Common::Vec3();
      if (!(t == RJSON_OBJECT ? ReadActionData(json, &feature->m_action_xyz) : Skip(json, t)))
        return false;
    }
    else if (!Skip(json, t))
    {
      return false;
    }
  }
  return t == RJSON_OBJECT_END;
}

/* { "name": ..., "data": { key: path, ... } } after its '{' */
bool ReadAsset(rjson_t* json, GraphicsModAssetConfig* asset)
{
  static const char* const names[] = {"name", "data"};
  enum rjson_type name_type = RJSON_DONE;
  enum rjson_type data_type = RJSON_DONE;
  std::string bad_key;
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 2);
    t = rjson_next(json);
    if (k == 0)
    {
      name_type = t;
      if (t == RJSON_STRING)
        asset->m_asset_id = Text(json);
    }
    else if (k == 1)
    {
      data_type = t;
      asset->m_map.clear();
      bad_key.clear();
      if (t == RJSON_OBJECT)
      {
        while ((t = rjson_next(json)) == RJSON_STRING)
        {
          std::string key(Text(json));
          t = rjson_next(json);
          if (t == RJSON_STRING)
            asset->m_map[key] = Text(json);
          else if (bad_key.empty())
            bad_key = std::move(key);
          if (!Skip(json, t))
            return false;
        }
        if (t != RJSON_OBJECT_END)
          return false;
        continue;
      }
    }
    if (!Skip(json, t))
      return false;
  }
  if (t != RJSON_OBJECT_END)
    return false;

  if (name_type == RJSON_DONE)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified asset has no name");
    return false;
  }
  if (name_type != RJSON_STRING)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified asset has a name "
                     "that is not a string");
    return false;
  }
  if (data_type == RJSON_DONE)
  {
    ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified asset '%s' has no data",
              asset->m_asset_id.c_str());
    return false;
  }
  if (data_type != RJSON_OBJECT)
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, specified asset '%s' has data "
              "that is not an object",
              asset->m_asset_id.c_str());
    return false;
  }
  if (!bad_key.empty())
  {
    ERROR_LOG(VIDEO,
              "Failed to load mod configuration file, specified asset '%s' has data "
              "with a value for key '%s' that is not a string",
              asset->m_asset_id.c_str(), bad_key.c_str());
    return false;
  }
  return true;
}

/* An array member of objects, each through read into a new element of out. */
template <typename T, typename F>
bool ReadObjects(rjson_t* json, enum rjson_type t, std::vector<T>* out, std::string_view what,
                 F read)
{
  if (t != RJSON_ARRAY)
    return Skip(json, t);
  out->clear();
  return ReadArray(json, [&](enum rjson_type e) {
    if (e != RJSON_OBJECT)
    {
      ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified %.*s is not a json object",
                static_cast<int>(what.size()), what.data());
      return false;
    }
    T item;
    if (!read(json, &item))
      return false;
    out->push_back(std::move(item));
    return true;
  });
}

/* A mod's metadata.json */
bool ReadConfig(rjson_t* json, GraphicsModConfig* mod)
{
  static const char* const names[] = {"meta", "groups", "features", "assets"};
  enum rjson_type t = rjson_next(json);
  if (t != RJSON_OBJECT)
    return Skip(json, t);
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 4);
    bool ok;
    t = rjson_next(json);
    switch (k)
    {
    case 0:
      ok = t == RJSON_OBJECT ? ReadMeta(json, mod) : Skip(json, t);
      break;
    case 1:
      ok = ReadObjects(json, t, &mod->m_groups, "group", ReadGroup);
      break;
    case 2:
      ok = ReadObjects(json, t, &mod->m_features, "feature", ReadFeature);
      break;
    case 3:
      ok = ReadObjects(json, t, &mod->m_assets, "asset", ReadAsset);
      break;
    default:
      ok = Skip(json, t);
      break;
    }
    if (!ok)
      return false;
  }
  return t == RJSON_OBJECT_END;
}

/* An array's element count after its '[' */
bool Count(rjson_t* json, long* n)
{
  *n = 0;
  return ReadArray(json, [&](enum rjson_type e) {
    ++*n;
    return Skip(json, e);
  });
}

/* A profile's mod entry after its '{': the mod it names, into mods where it loads. */
bool ReadProfileMod(rjson_t* json, std::vector<GraphicsModConfig>* mods)
{
  static const char* const names[] = {"source", "path", "groups", "features", "enabled",
                                      "weight"};
  GraphicsTargetField source, path;
  long groups = -1, features = -1;
  std::optional<bool> enabled;
  std::optional<double> weight;
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 6);
    bool ok = true;
    t = rjson_next(json);
    switch (k)
    {
    case 0:
      ok = ReadField(json, t, &source);
      break;
    case 1:
      ok = ReadField(json, t, &path);
      break;
    case 2:
    case 3:
    {
      long* const n = k == 2 ? &groups : &features;
      *n = -1;
      ok = t == RJSON_ARRAY ? Count(json, n) : Skip(json, t);
      break;
    }
    case 4:
      enabled.reset();
      if (t == RJSON_TRUE || t == RJSON_FALSE)
        enabled = t == RJSON_TRUE;
      ok = Skip(json, t);
      break;
    case 5:
      weight.reset();
      if (t == RJSON_NUMBER)
        weight = rjson_get_double(json);
      ok = Skip(json, t);
      break;
    default:
      ok = Skip(json, t);
      break;
    }
    if (!ok)
      return false;
  }
  if (t != RJSON_OBJECT_END)
    return false;

  if (source.kind == GraphicsTargetField::Kind::Absent ||
      path.kind != GraphicsTargetField::Kind::String)
  {
    return true;
  }
  auto mod = source.kind == GraphicsTargetField::Kind::String && source.text == "system" ?
                 GraphicsModConfig::Create(File::GetSysDirectory() +
                                               DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR + path.text,
                                           GraphicsModConfig::Source::System) :
                 GraphicsModConfig::Create(File::GetUserPath(D_GRAPHICSMOD_IDX) + path.text,
                                           GraphicsModConfig::Source::User);
  if (!mod)
    return true;
  if ((groups < 0 || static_cast<size_t>(groups) == mod->m_groups.size()) &&
      (features < 0 || static_cast<size_t>(features) == mod->m_features.size()))
  {
    if (enabled)
      mod->m_enabled = *enabled;
    if (weight)
      mod->m_weight = static_cast<u16>(*weight);
  }
  mods->push_back(std::move(*mod));
  return true;
}
}  // namespace

std::optional<GraphicsModConfig> GraphicsModConfig::Create(const std::string& file_path,
                                                           Source source)
{
  GraphicsModConfig result;
  {
    Document doc(file_path);
    if (!doc.json)
    {
      ERROR_LOG(VIDEO, "Failed to load graphics mod json file '%s'", file_path.c_str());
      return std::nullopt;
    }
    if (!ReadConfig(doc.json, &result))
    {
      if (*rjson_get_error(doc.json))
        LogParseError(doc.json, "graphics mod", file_path);
      return std::nullopt;
    }
  }
  result.m_source = source;
  if (source == Source::User)
  {
    const std::string base_path = File::GetUserPath(D_GRAPHICSMOD_IDX);
    if (base_path.size() > file_path.size())
    {
      ERROR_LOG(
          VIDEO,
          "Failed to load graphics mod json file '%s' due to it not matching the base path: %s",
          file_path.c_str(), base_path.c_str());
      return std::nullopt;
    }
    result.m_relative_path = file_path.substr(base_path.size());
  }
  else
  {
    const std::string base_path = File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR;
    if (base_path.size() > file_path.size())
    {
      ERROR_LOG(
          VIDEO,
          "Failed to load graphics mod json file '%s' due to it not matching the base path: %s",
          file_path.c_str(), base_path.c_str());
      return std::nullopt;
    }
    result.m_relative_path = file_path.substr(base_path.size());
  }

  return result;
}

std::string GraphicsModConfig::GetAbsolutePath() const
{
  if (m_source == Source::System)
  {
    return WithUnifiedPathSeparators(File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR +
                                     m_relative_path);
  }
  else
  {
    return WithUnifiedPathSeparators(File::GetUserPath(D_GRAPHICSMOD_IDX) + m_relative_path);
  }
}

std::optional<std::vector<GraphicsModConfig>> LoadGraphicsModProfile(const std::string& file_path)
{
  Document doc(file_path);
  if (!doc.json)
  {
    ERROR_LOG(VIDEO, "Failed to load graphics mod group json file '%s'", file_path.c_str());
    return std::nullopt;
  }
  enum rjson_type t = rjson_next(doc.json);
  if (t != RJSON_OBJECT)
  {
    if (t == RJSON_ERROR)
      LogParseError(doc.json, "graphics mod group", file_path);
    else
      ERROR_LOG(VIDEO,
                "Failed to load graphics mod group json file '%s' due to root not being an object!",
                file_path.c_str());
    return std::nullopt;
  }

  static const char* const names[] = {"mods"};
  std::vector<GraphicsModConfig> mods;
  while ((t = rjson_next(doc.json)) == RJSON_STRING)
  {
    const int k = Key(doc.json, names, 1);
    bool ok;
    t = rjson_next(doc.json);
    if (k == 0 && t == RJSON_ARRAY)
    {
      mods.clear();
      ok = ReadArray(doc.json, [&](enum rjson_type e) {
        return e == RJSON_OBJECT ? ReadProfileMod(doc.json, &mods) : Skip(doc.json, e);
      });
    }
    else
    {
      ok = Skip(doc.json, t);
    }
    if (!ok)
      break;
  }
  if (t != RJSON_OBJECT_END)
  {
    LogParseError(doc.json, "graphics mod group", file_path);
    return std::nullopt;
  }
  return mods;
}
