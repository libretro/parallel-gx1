// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/HiresTextures.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VideoConfig.h"
#include "Core/Core.h"

constexpr std::string_view s_format_prefix{"tex1_"};

// The pack's textures by name (file name less "_arb" and extension), with whether they carry
// arbitrary mipmaps; each one's HiresTexture made on its first use.
struct HiresEntry
{
  char* name;
  bool has_arbitrary_mipmaps;
  HiresTexture* texture;
};
static HiresEntry* s_entries;
static u32 s_num_entries;
static u32 s_entries_capacity;
// Open-addressed: name hash to entry index, NO_ENTRY where free; a power of two in size.
static constexpr u32 NO_ENTRY = 0xFFFFFFFFu;
static u32* s_index;
static u32 s_index_capacity;
static u32 s_num_preloaded;

// One reference held here, one by each asset made from it
static VideoCommon::DirectFilesystemAssetLibrary* s_file_library;

static VideoCommon::DirectFilesystemAssetLibrary* FileLibrary()
{
  if (!s_file_library)
    s_file_library = new VideoCommon::DirectFilesystemAssetLibrary();
  return s_file_library;
}

static u32 HashName(const char* name)
{
  // FNV-1a
  u32 hash = 2166136261u;
  for (; *name; name++)
    hash = (hash ^ static_cast<u8>(*name)) * 16777619u;
  return hash;
}

// The index slot of name: its entry's, or the free one it would take
static u32 FindSlot(const char* name)
{
  const u32 mask = s_index_capacity - 1;
  u32 i = HashName(name) & mask;
  while (s_index[i] != NO_ENTRY && strcmp(s_entries[s_index[i]].name, name) != 0)
    i = (i + 1) & mask;
  return i;
}

static HiresEntry* FindEntry(const char* name)
{
  if (!s_num_entries)
    return nullptr;
  const u32 index = s_index[FindSlot(name)];
  return index == NO_ENTRY ? nullptr : &s_entries[index];
}

// The new entry for name, null when name has one already
static HiresEntry* AddEntry(const char* name, bool has_arbitrary_mipmaps)
{
  // keep the index at most half full
  if ((s_num_entries + 1) * 2 > s_index_capacity)
  {
    const u32 capacity = s_index_capacity ? s_index_capacity * 2 : 256;
    u32* const index = static_cast<u32*>(malloc(capacity * sizeof(u32)));
    if (!index)
      abort();
    memset(index, 0xFF, capacity * sizeof(u32));
    free(s_index);
    s_index = index;
    s_index_capacity = capacity;
    for (u32 e = 0; e < s_num_entries; e++)
      s_index[FindSlot(s_entries[e].name)] = e;
  }
  const u32 slot = FindSlot(name);
  if (s_index[slot] != NO_ENTRY)
    return nullptr;
  if (s_num_entries == s_entries_capacity)
  {
    const u32 capacity = s_entries_capacity ? s_entries_capacity * 2 : 128;
    HiresEntry* const entries =
        static_cast<HiresEntry*>(realloc(s_entries, capacity * sizeof(HiresEntry)));
    if (!entries)
      abort();
    s_entries = entries;
    s_entries_capacity = capacity;
  }
  HiresEntry* const entry = &s_entries[s_num_entries];
  entry->name = strdup(name);
  if (!entry->name)
    abort();
  entry->has_arbitrary_mipmaps = has_arbitrary_mipmaps;
  entry->texture = nullptr;
  s_index[slot] = s_num_entries++;
  return entry;
}

static HiresTexture* EntryTexture(HiresEntry* entry)
{
  if (!entry->texture)
    entry->texture = new HiresTexture(entry->has_arbitrary_mipmaps, entry->name);
  return entry->texture;
}

namespace
{
HiresEntry* FindEntryFor(const TextureInfo& texture_info)
{
  if (!s_num_entries)
    return nullptr;

  const TextureInfo::NameDetails details = texture_info.CalculateTextureName();
  if (!details.base_name[0])
    return nullptr;
  char name[TextureInfo::NAME_SIZE];

  // look for an exact match first
  details.GetFullName(name, sizeof(name));
  if (HiresEntry* entry = FindEntry(name))
    return entry;

  // Single wildcard ignoring the tlut hash
  snprintf(name, sizeof(name), "%s_%s_$_%s", details.base_name, details.texture_name,
           details.format_name);
  if (HiresEntry* entry = FindEntry(name))
    return entry;

  // Single wildcard ignoring the texture hash
  snprintf(name, sizeof(name), "%s_$%s_%s", details.base_name, details.tlut_name,
           details.format_name);
  return FindEntry(name);
}
}  // namespace

void HiresTexture::Shutdown()
{
  Clear();
}

void HiresTexture::Update()
{
  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  path_list_t texture_directories;
  path_list_init(&texture_directories);
  GetTextureDirectoriesForFirstMatchingGameId(File::GetUserPath(D_HIRESTEXTURES_IDX).c_str(),
                                              SConfig::GetInstance().GetGameIDsForTextures(),
                                              &texture_directories);

  static const char* const extensions[] = {".png", ".dds"};

  path_list_t texture_paths;
  path_list_init(&texture_paths);
  for (unsigned dir_index = 0; dir_index < texture_directories.count; dir_index++)
  {
    const char* const texture_directory_path = path_list_get(&texture_directories, dir_index);
    path_list_clear(&texture_paths);
    Common::DoFileSearch(&texture_directory_path, 1, extensions,
                         sizeof(extensions) / sizeof(extensions[0]), /*recursive*/ true,
                         &texture_paths);

    bool failed_insert = false;
    for (unsigned i = 0; i < texture_paths.count; i++)
    {
      const std::string path = path_list_get(&texture_paths, i);
      std::string filename;
      SplitPath(path, nullptr, &filename, nullptr);

      if (filename.substr(0, s_format_prefix.length()) == s_format_prefix)
      {
        const size_t arb_index = filename.rfind("_arb");
        const bool has_arbitrary_mipmaps = arb_index != std::string::npos;
        if (has_arbitrary_mipmaps)
          filename.erase(arb_index, 4);

        HiresEntry* const entry = AddEntry(filename.c_str(), has_arbitrary_mipmaps);
        if (!entry)
        {
          failed_insert = true;
        }
        else
        {
          // Since this is just a texture (single file) the mapper doesn't really matter
          // just provide a string
          FileLibrary()->SetAssetIDMapData(filename,
                                           std::map<std::string, std::string>{{"texture", path}});

          if (g_ActiveConfig.bCacheHiresTextures)
          {
            static_cast<void>(EntryTexture(entry)->LoadTexture());
            s_num_preloaded++;
          }
        }
      }
    }

    if (failed_insert)
    {
      ERROR_LOG(VIDEO, "One or more textures at path '%s' were already inserted",
                texture_directory_path);
    }
  }
  path_list_free(&texture_paths);
  path_list_free(&texture_directories);

  const std::vector<std::string> game_ids_for_textures =
      SConfig::GetInstance().GetGameIDsForTextures();
  std::string game_id_display;
  for (const std::string& game_id : game_ids_for_textures)
  {
    if (!game_id_display.empty())
      game_id_display += "' or '";
    game_id_display += game_id;
  }

  if (g_ActiveConfig.bCacheHiresTextures)
  {
    Core::DisplayMessage("Preloading '%u' custom textures for '%s'",
                         static_cast<unsigned>(s_num_preloaded), game_id_display.c_str());
  }
  else
  {
    Core::DisplayMessage("Found '%u' custom textures for '%s'",
                         static_cast<unsigned>(s_num_entries), game_id_display.c_str());
  }
}

void HiresTexture::Clear()
{
  for (u32 e = 0; e < s_num_entries; e++)
  {
    delete s_entries[e].texture;
    free(s_entries[e].name);
  }
  free(s_entries);
  free(s_index);
  s_entries = nullptr;
  s_index = nullptr;
  s_num_entries = s_entries_capacity = s_index_capacity = 0;
  s_num_preloaded = 0;
  if (s_file_library)
    s_file_library->Release();
  s_file_library = new VideoCommon::DirectFilesystemAssetLibrary();
}

HiresTexture* HiresTexture::Search(const TextureInfo& texture_info)
{
  HiresEntry* const entry = FindEntryFor(texture_info);
  return entry ? EntryTexture(entry) : nullptr;
}

HiresTexture::HiresTexture(bool has_arbitrary_mipmaps, std::string id)
    : m_has_arbitrary_mipmaps(has_arbitrary_mipmaps), m_id(std::move(id))
{
}

VideoCommon::TextureDataResource* HiresTexture::LoadTexture() const
{
  auto& system = Core::System::GetInstance();
  auto& custom_resource_manager = system.GetCustomResourceManager();
  return custom_resource_manager.GetTextureDataFromAsset(m_id, FileLibrary());
}

// Whether the name of the .txt file at path, without its extension, is the game id, its first
// three characters or "all".
static bool MatchesGameIdOrAll(const char* path, size_t length, const char* game_id)
{
  const size_t id_length = std::strlen(game_id);
  const size_t region_free_length = id_length < 3 ? id_length : 3;
  size_t name_start = length;
  size_t name_length;
  while (name_start > 0 && path[name_start - 1] != '/'
#ifdef _WIN32
         && path[name_start - 1] != ':'
#endif
  )
  {
    name_start--;
  }
  // the extension starts at the last '.', which DoFileSearch's ".txt" match puts 4 from the end
  name_length = length - 4 - name_start;
  path += name_start;
  return (name_length == id_length && !std::memcmp(path, game_id, id_length)) ||
         (name_length == region_free_length && !std::memcmp(path, game_id, region_free_length)) ||
         (name_length == 3 && !std::memcmp(path, "all", 3));
}

void GetTextureDirectoriesWithGameId(const char* root_directory, const char* game_id,
                                     path_list_t* result)
{
  const size_t root_length = std::strlen(root_directory);
  const size_t id_length = std::strlen(game_id);
  char* const directory = static_cast<char*>(std::malloc(root_length + id_length + 1));
  if (!directory)
    return;
  std::memcpy(directory, root_directory, root_length);
  std::memcpy(directory + root_length, game_id, id_length + 1);

  if (File::Exists(directory))
  {
    path_list_add(result, directory);
  }
  else
  {
    // If there's no directory with the region-specific ID, look for a 3-character region-free one
    directory[root_length + (id_length < 3 ? id_length : 3)] = '\0';
    if (File::Exists(directory))
      path_list_add(result, directory);
  }
  std::free(directory);

  // Look for any other directories that might be specific to the given gameid
  path_list_t files;
  path_list_init(&files);
  Common::DoFileSearch(root_directory, ".txt", true, &files);
  for (unsigned i = 0; i < files.count; i++)
  {
    const char* const file = path_list_get(&files, i);
    const size_t file_length = std::strlen(file);
    if (file_length < root_length || !MatchesGameIdOrAll(file, file_length, game_id))
      continue;

    // The top directory of a found gameid.txt file
    // ex:  <root directory>/My folder/gameids/<gameid>.txt
    // adds "<root directory>/My folder"
    const char* const below_root = file + root_length;
    const char* const separator = std::strchr(below_root, DIR_SEP_CHR);
    const size_t below_length =
        separator ? static_cast<size_t>(separator - below_root) : std::strlen(below_root);
    char* const top = static_cast<char*>(std::malloc(root_length + below_length + 1));
    if (!top)
      continue;
    std::memcpy(top, root_directory, root_length);
    std::memcpy(top + root_length, below_root, below_length);
    top[root_length + below_length] = '\0';
    path_list_add(result, top);
    std::free(top);
  }
  path_list_free(&files);

  path_list_sort_unique(result);
}

void GetTextureDirectoriesForFirstMatchingGameId(const char* root_directory,
                                                 const std::vector<std::string>& game_ids,
                                                 path_list_t* result)
{
  for (const std::string& game_id : game_ids)
  {
    path_list_clear(result);
    GetTextureDirectoriesWithGameId(root_directory, game_id.c_str(), result);
    if (result->count)
      return;
  }
}
