// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"

#include <cinttypes>
#include <cstdlib>
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
// A grown array's new zeroed last element, or nullptr when out of memory
void* AddElement(void** elements, u32* count, u32* capacity, size_t size)
{
  if (*count == *capacity)
  {
    const u32 new_capacity = *capacity ? *capacity * 2 : 4;
    void* const grown = std::realloc(*elements, new_capacity * size);
    if (!grown)
      return nullptr;
    *elements = grown;
    *capacity = new_capacity;
  }
  char* const element = static_cast<char*>(*elements) + size * (*count)++;
  std::memset(element, 0, size);
  return element;
}

GraphicsTargetConfig* AddTarget(GraphicsTargetGroupConfig* group)
{
  return static_cast<GraphicsTargetConfig*>(
      AddElement(reinterpret_cast<void**>(&group->m_targets), &group->m_target_count,
                 &group->m_target_capacity, sizeof(GraphicsTargetConfig)));
}

GraphicsTargetGroupConfig* AddGroup(GraphicsModConfig* mod)
{
  return static_cast<GraphicsTargetGroupConfig*>(
      AddElement(reinterpret_cast<void**>(&mod->m_groups), &mod->m_group_count,
                 &mod->m_group_capacity, sizeof(GraphicsTargetGroupConfig)));
}

GraphicsModFeatureConfig* AddFeature(GraphicsModConfig* mod)
{
  return static_cast<GraphicsModFeatureConfig*>(
      AddElement(reinterpret_cast<void**>(&mod->m_features), &mod->m_feature_count,
                 &mod->m_feature_capacity, sizeof(GraphicsModFeatureConfig)));
}

void ClearTargets(GraphicsTargetGroupConfig* group)
{
  for (u32 i = 0; i < group->m_target_count; i++)
    FreeGraphicsTarget(&group->m_targets[i]);
  group->m_target_count = 0;
}

void ClearGroups(GraphicsModConfig* mod)
{
  for (u32 i = 0; i < mod->m_group_count; i++)
  {
    ClearTargets(&mod->m_groups[i]);
    std::free(mod->m_groups[i].m_targets);
    std::free(mod->m_groups[i].m_name);
  }
  mod->m_group_count = 0;
}

void ClearFeatures(GraphicsModConfig* mod)
{
  for (u32 i = 0; i < mod->m_feature_count; i++)
  {
    std::free(mod->m_features[i].m_group);
    std::free(mod->m_features[i].m_action);
  }
  mod->m_feature_count = 0;
}

void ClearMods(GraphicsModList* list)
{
  for (u32 i = 0; i < list->count; i++)
    FreeGraphicsModConfig(&list->mods[i]);
  list->count = 0;
}

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

// The current string, copied; false when out of memory
bool CopyText(rjson_t* json, char** out)
{
  const std::string_view text = Text(json);
  char* const copy = static_cast<char*>(std::malloc(text.size() + 1));
  if (!copy)
    return false;
  std::memcpy(copy, text.data(), text.size());
  copy[text.size()] = '\0';
  std::free(*out);
  *out = copy;
  return true;
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
    return CopyText(json, &field->text);
  }
  FreeGraphicsTargetField(field);
  field->kind = GraphicsTargetField::Kind::Other;
  return Skip(json, t);
}

/* A string member into out, or false with what it was logged. */
bool ReadString(rjson_t* json, enum rjson_type t, char** out, std::string_view what)
{
  if (t == RJSON_STRING)
    return CopyText(json, out);
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

/* { "title": ..., ... } after its '{'; the author and description are not used */
bool ReadMeta(rjson_t* json, GraphicsModConfig* mod)
{
  static const char* const names[] = {"title"};
  enum rjson_type t;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 1);
    t = rjson_next(json);
    if (k == 0 && t == RJSON_STRING)
    {
      if (!CopyText(json, &mod->m_title))
        return false;
    }
    else if (!Skip(json, t))
    {
      return false;
    }
  }
  return t == RJSON_OBJECT_END;
}

/* { "type": ..., "texture_filename": ..., "value": ... } after its '{' */
bool ReadTarget(rjson_t* json, GraphicsTargetGroupConfig* group)
{
  static const char* const names[] = {"type", "texture_filename", "value"};
  GraphicsTargetField fields[3] = {};
  enum rjson_type t;
  bool ok = true;
  while (ok && (t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    t = rjson_next(json);
    ok = k >= 0 ? ReadField(json, t, &fields[k]) : Skip(json, t);
  }
  ok = ok && t == RJSON_OBJECT_END;
  if (ok)
  {
    GraphicsTargetConfig* const target = AddTarget(group);
    if (!target || !MakeTarget(fields[0], fields[1], fields[2], target))
    {
      if (target)
        FreeGraphicsTarget(&group->m_targets[--group->m_target_count]);
      ok = false;
    }
  }
  for (u32 i = 0; i < 3; i++)
    FreeGraphicsTargetField(&fields[i]);
  return ok;
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
      ClearTargets(group);
      if (!ReadArray(json, [&](enum rjson_type e) {
            if (e != RJSON_OBJECT)
            {
              ERROR_LOG(VIDEO, "Failed to load shader configuration file, specified target "
                               "is not a json object");
              return false;
            }
            return ReadTarget(json, group);
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

/* An array member of objects, each through read into the element add makes; clear empties the
 * list first. A failed element stays, for the mod's free. */
template <typename T>
bool ReadObjects(rjson_t* json, enum rjson_type t, GraphicsModConfig* mod,
                 void (*clear)(GraphicsModConfig*), T* (*add)(GraphicsModConfig*),
                 std::string_view what, bool (*read)(rjson_t*, T*))
{
  if (t != RJSON_ARRAY)
    return Skip(json, t);
  clear(mod);
  return ReadArray(json, [&](enum rjson_type e) {
    if (e != RJSON_OBJECT)
    {
      ERROR_LOG(VIDEO, "Failed to load mod configuration file, specified %.*s is not a json object",
                static_cast<int>(what.size()), what.data());
      return false;
    }
    T* const item = add(mod);
    return item && read(json, item);
  });
}

/* A mod's metadata.json */
bool ReadConfig(rjson_t* json, GraphicsModConfig* mod)
{
  static const char* const names[] = {"meta", "groups", "features"};
  enum rjson_type t = rjson_next(json);
  if (t != RJSON_OBJECT)
    return Skip(json, t);
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 3);
    bool ok;
    t = rjson_next(json);
    switch (k)
    {
    case 0:
      ok = t == RJSON_OBJECT ? ReadMeta(json, mod) : Skip(json, t);
      break;
    case 1:
      ok = ReadObjects(json, t, mod, ClearGroups, AddGroup, "group", ReadGroup);
      break;
    case 2:
      ok = ReadObjects(json, t, mod, ClearFeatures, AddFeature, "feature", ReadFeature);
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
bool ReadProfileMod(rjson_t* json, GraphicsModList* mods)
{
  static const char* const names[] = {"source", "path", "groups", "features", "enabled",
                                      "weight"};
  GraphicsTargetField source = {};
  GraphicsTargetField path = {};
  long groups = -1, features = -1;
  bool has_enabled = false, enabled = false;
  bool has_weight = false;
  double weight = 0;
  enum rjson_type t;
  bool ok = true;
  while ((t = rjson_next(json)) == RJSON_STRING)
  {
    const int k = Key(json, names, 6);
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
      has_enabled = t == RJSON_TRUE || t == RJSON_FALSE;
      enabled = t == RJSON_TRUE;
      ok = Skip(json, t);
      break;
    case 5:
      has_weight = t == RJSON_NUMBER;
      if (has_weight)
        weight = rjson_get_double(json);
      ok = Skip(json, t);
      break;
    default:
      ok = Skip(json, t);
      break;
    }
    if (!ok)
      break;
  }
  bool read = ok && t == RJSON_OBJECT_END;

  if (read && source.kind != GraphicsTargetField::Kind::Absent &&
      path.kind == GraphicsTargetField::Kind::String)
  {
    GraphicsModConfig* const mod = AddGraphicsMod(mods);
    const bool is_system =
        source.kind == GraphicsTargetField::Kind::String && !std::strcmp(source.text, "system");
    const bool created =
        mod &&
        (is_system ? GraphicsModConfig::Create(File::GetSysDirectory() +
                                                   DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR + path.text,
                                               GraphicsModConfig::Source::System, mod) :
                     GraphicsModConfig::Create(File::GetUserPath(D_GRAPHICSMOD_IDX) + path.text,
                                               GraphicsModConfig::Source::User, mod));
    if (!created)
    {
      if (mod)
        FreeGraphicsModConfig(&mods->mods[--mods->count]);
    }
    else if ((groups < 0 || static_cast<u32>(groups) == mod->m_group_count) &&
             (features < 0 || static_cast<u32>(features) == mod->m_feature_count))
    {
      if (has_enabled)
        mod->m_enabled = enabled;
      if (has_weight)
        mod->m_weight = static_cast<u16>(weight);
    }
  }
  FreeGraphicsTargetField(&source);
  FreeGraphicsTargetField(&path);
  return read;
}
}  // namespace

bool GraphicsModConfig::Create(const std::string& file_path, Source source,
                               GraphicsModConfig* config)
{
  FreeGraphicsModConfig(config);
  GraphicsModConfig& result = *config;
  {
    Document doc(file_path);
    if (!doc.json)
    {
      ERROR_LOG(VIDEO, "Failed to load graphics mod json file '%s'", file_path.c_str());
      return false;
    }
    if (!ReadConfig(doc.json, &result))
    {
      if (*rjson_get_error(doc.json))
        LogParseError(doc.json, "graphics mod", file_path);
      return false;
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
      return false;
    }
    result.m_relative_path = strdup(file_path.c_str() + base_path.size());
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
      return false;
    }
    result.m_relative_path = strdup(file_path.c_str() + base_path.size());
  }

  return result.m_relative_path != nullptr;
}

std::string GraphicsModConfig::GetAbsolutePath() const
{
  if (m_source == Source::System)
  {
    return WithUnifiedPathSeparators(File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR +
                                     (m_relative_path ? m_relative_path : ""));
  }
  else
  {
    return WithUnifiedPathSeparators(File::GetUserPath(D_GRAPHICSMOD_IDX) +
                                     (m_relative_path ? m_relative_path : ""));
  }
}

void FreeGraphicsModConfig(GraphicsModConfig* config)
{
  ClearGroups(config);
  ClearFeatures(config);
  std::free(config->m_groups);
  std::free(config->m_features);
  std::free(config->m_title);
  std::free(config->m_relative_path);
  std::memset(config, 0, sizeof(*config));
}

GraphicsModConfig* AddGraphicsMod(GraphicsModList* list)
{
  return static_cast<GraphicsModConfig*>(AddElement(reinterpret_cast<void**>(&list->mods),
                                                    &list->count, &list->capacity,
                                                    sizeof(GraphicsModConfig)));
}

void FreeGraphicsModList(GraphicsModList* list)
{
  ClearMods(list);
  std::free(list->mods);
  list->mods = nullptr;
  list->capacity = 0;
}

bool LoadGraphicsModProfile(const std::string& file_path, GraphicsModList* mods_out)
{
  Document doc(file_path);
  if (!doc.json)
  {
    ERROR_LOG(VIDEO, "Failed to load graphics mod group json file '%s'", file_path.c_str());
    return false;
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
    return false;
  }

  static const char* const names[] = {"mods"};
  GraphicsModList* const mods = mods_out;
  while ((t = rjson_next(doc.json)) == RJSON_STRING)
  {
    const int k = Key(doc.json, names, 1);
    bool ok;
    t = rjson_next(doc.json);
    if (k == 0 && t == RJSON_ARRAY)
    {
      ClearMods(mods);
      ok = ReadArray(doc.json, [&](enum rjson_type e) {
        return e == RJSON_OBJECT ? ReadProfileMod(doc.json, mods) : Skip(doc.json, e);
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
    return false;
  }
  return true;
}
