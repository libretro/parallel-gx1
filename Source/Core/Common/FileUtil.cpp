// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FileUtil.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#ifdef ANDROID
#include "Common/Assert.h"
#endif
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace File
{
#ifdef ANDROID
static std::string s_android_sys_directory;
#else
static std::string s_libretro_sys_directory;
#endif

FileInfo::FileInfo(const std::string& path) : FileInfo(path.c_str())
{
}

FileInfo::FileInfo(const char* path)
{
  const int flags = path_stat(path);
  m_exists = (flags & RETRO_VFS_STAT_IS_VALID) != 0;
  m_is_directory = (flags & RETRO_VFS_STAT_IS_DIRECTORY) != 0;
  if (m_exists && !m_is_directory)
  {
    const int64_t size = path_get_size(path);
    m_size = size > 0 ? u64(size) : 0;
  }
}

bool Exists(const char* path)
{
  return path_is_valid(path);
}

bool IsDirectory(const char* path)
{
  return path_is_directory(path);
}

bool IsFile(const char* path)
{
  const int flags = path_stat(path);
  return (flags & RETRO_VFS_STAT_IS_VALID) && !(flags & RETRO_VFS_STAT_IS_DIRECTORY);
}

u64 GetSize(const char* path)
{
  int64_t size;
  if (!IsFile(path))
    return 0;
  size = path_get_size(path);
  return size > 0 ? u64(size) : 0;
}

bool Delete(const char* filename, IfAbsentBehavior behavior)
{
  const int flags = path_stat(filename);

  DEBUG_LOG_FMT(COMMON, "{}: file {}", __func__, filename);

  // Return true because we care about the file not being there, not the actual delete.
  if (!(flags & RETRO_VFS_STAT_IS_VALID))
  {
    if (behavior == IfAbsentBehavior::ConsoleWarning)
      WARN_LOG_FMT(COMMON, "{}: {} does not exist", __func__, filename);
    return true;
  }

  if (flags & RETRO_VFS_STAT_IS_DIRECTORY)
  {
    WARN_LOG_FMT(COMMON, "{} failed: {} is a directory", __func__, filename);
    return false;
  }

  if (filestream_delete(filename) != 0)
  {
    WARN_LOG_FMT(COMMON, "{}: failed on {}", __func__, filename);
    return false;
  }
  return true;
}

bool CreateDir(const char* path)
{
  DEBUG_LOG_FMT(COMMON, "{}: directory {}", __func__, path);
  if (path_mkdir(path))
    return true;
  ERROR_LOG_FMT(COMMON, "{}: failed on {}", __func__, path);
  return false;
}

bool CreateDirs(std::string_view path)
{
  char buf[PATH_MAX_LENGTH];
  if (path.size() >= sizeof(buf))
    return false;
  std::memcpy(buf, path.data(), path.size());
  buf[path.size()] = '\0';
  return CreateDir(buf);
}

bool CreateFullPath(std::string_view fullPath)
{
  char buf[PATH_MAX_LENGTH];
  size_t slash = fullPath.size();

  DEBUG_LOG_FMT(COMMON, "{}: path {}", __func__, fullPath);
  // the directory part: up to the last separator
  while (slash && fullPath[slash - 1] != '/'
#ifdef _WIN32
         && fullPath[slash - 1] != '\\'
#endif
  )
  {
    slash--;
  }
  if (slash <= 1)
    return true;
  slash--;
  if (slash >= sizeof(buf))
    return false;
  std::memcpy(buf, fullPath.data(), slash);
  buf[slash] = '\0';
  return CreateDir(buf);
}

bool DeleteDir(const char* filename, IfAbsentBehavior behavior)
{
  const int flags = path_stat(filename);

  DEBUG_LOG_FMT(COMMON, "{}: directory {}", __func__, filename);

  // Return true because we care about the directory not being there, not the actual delete.
  if (!(flags & RETRO_VFS_STAT_IS_VALID))
  {
    if (behavior == IfAbsentBehavior::ConsoleWarning)
      WARN_LOG_FMT(COMMON, "{}: {} does not exist", __func__, filename);
    return true;
  }

  if (!(flags & RETRO_VFS_STAT_IS_DIRECTORY))
  {
    ERROR_LOG_FMT(COMMON, "{}: Not a directory {}", __func__, filename);
    return false;
  }

  if (!path_rmdir(filename))
  {
    WARN_LOG_FMT(COMMON, "{}: failed on {}", __func__, filename);
    return false;
  }
  return true;
}

// Replaces destFilename if it exists.
bool Rename(const char* srcFilename, const char* destFilename)
{
  DEBUG_LOG_FMT(COMMON, "{}: {} --> {}", __func__, srcFilename, destFilename);
  if (filestream_rename(srcFilename, destFilename) == 0)
    return true;
  // a frontend VFS may refuse to replace
  if (IsFile(destFilename) && filestream_delete(destFilename) == 0 &&
      filestream_rename(srcFilename, destFilename) == 0)
  {
    return true;
  }
  ERROR_LOG_FMT(COMMON, "{} failed: {} --> {}", __func__, srcFilename, destFilename);
  return false;
}

bool CopyRegularFile(std::string_view source_path, std::string_view destination_path)
{
  const std::string src(source_path);
  const std::string dst(destination_path);

  DEBUG_LOG_FMT(COMMON, "{}: {} --> {}", __func__, source_path, destination_path);
  if (filestream_copy(src.c_str(), dst.c_str()) == 0)
    return true;
  ERROR_LOG_FMT(COMMON, "{}: failed {} --> {}", __func__, source_path, destination_path);
  return false;
}

bool CreateEmptyFile(const std::string& filename)
{
  DEBUG_LOG_FMT(COMMON, "CreateEmptyFile: {}", filename);
  if (!File::IOFile(filename, "wb"))
  {
    ERROR_LOG_FMT(COMMON, "CreateEmptyFile: failed {}", filename);
    return false;
  }
  return true;
}

static bool IsDotEntry(const char* name)
{
  return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}

static void ScanInto(FSTEntry* parent, bool recursive)
{
  struct RDIR* dir = retro_opendir_include_hidden(parent->physicalName.c_str(), true);
  size_t i;

  if (!dir)
    return;
  while (retro_readdir(dir))
  {
    const char* name = retro_dirent_get_name(dir);
    FSTEntry entry;
    int64_t size = 0;

    if (!name || IsDotEntry(name))
      continue;
    entry.isDirectory = retro_dirent_is_dir(dir, nullptr);
    if (!entry.isDirectory && retro_dirent_stat(dir, &size, nullptr) && size > 0)
      entry.size = u64(size);
    entry.virtualName = name;
    entry.physicalName = parent->physicalName;
    if (!entry.physicalName.empty() && entry.physicalName.back() != DIR_SEP_CHR)
      entry.physicalName += DIR_SEP_CHR;
    entry.physicalName += name;
    parent->children.push_back(std::move(entry));
  }
  retro_closedir(dir);

  // A directory's size is the recursive count of its children.
  parent->size += parent->children.size();
  for (i = 0; i < parent->children.size(); i++)
  {
    FSTEntry& child = parent->children[i];
    if (!child.isDirectory)
      continue;
    if (recursive)
      ScanInto(&child, true);
    parent->size += child.size;
  }
}

FSTEntry ScanDirectoryTree(const std::string& directory, bool recursive)
{
  FSTEntry parent_entry;

  DEBUG_LOG_FMT(COMMON, "{}: directory {}", __func__, directory);
  parent_entry.physicalName = directory;
  while (parent_entry.physicalName.size() > 1 && parent_entry.physicalName.back() == DIR_SEP_CHR)
    parent_entry.physicalName.pop_back();
  parent_entry.isDirectory = IsDirectory(directory);
  if (parent_entry.isDirectory)
    ScanInto(&parent_entry, recursive);
  return parent_entry;
}

static bool DeleteTree(const FSTEntry& dir)
{
  bool ok = true;
  for (const FSTEntry& child : dir.children)
  {
    if (child.isDirectory)
      ok = DeleteTree(child) && ok;
    else
      ok = filestream_delete(child.physicalName.c_str()) == 0 && ok;
  }
  return path_rmdir(dir.physicalName.c_str()) && ok;
}

bool DeleteDirRecursively(const std::string& directory)
{
  DEBUG_LOG_FMT(COMMON, "{}: {}", __func__, directory);
  if (!IsDirectory(directory))
    return true;
  if (DeleteTree(ScanDirectoryTree(directory, true)))
    return true;
  ERROR_LOG_FMT(COMMON, "{}: {} failed", __func__, directory);
  return false;
}

static bool CopyTree(const FSTEntry& src, const std::string& dst, bool overwrite_existing)
{
  bool ok = CreateDir(dst);
  for (const FSTEntry& child : src.children)
  {
    const std::string to = dst + DIR_SEP_CHR + child.virtualName;
    if (child.isDirectory)
      ok = CopyTree(child, to, overwrite_existing) && ok;
    else if (overwrite_existing || !Exists(to))
      ok = CopyRegularFile(child.physicalName, to) && ok;
  }
  return ok;
}

bool Copy(std::string_view source_path, std::string_view dest_path, bool overwrite_existing)
{
  const std::string src(source_path);
  const std::string dst(dest_path);

  DEBUG_LOG_FMT(COMMON, "{}: {} --> {} ({})", __func__, source_path, dest_path,
                overwrite_existing ? "overwrite" : "preserve");
  if (src == dst)
    return true;
  if (IsDirectory(src))
    return CopyTree(ScanDirectoryTree(src, true), dst, overwrite_existing);
  if (!overwrite_existing && Exists(dst))
    return true;
  return CopyRegularFile(src, dst);
}

static bool MoveTree(const std::string& src, const std::string& dst)
{
  // a rename moves it whole when the destination does not stand in the way
  if (filestream_rename(src.c_str(), dst.c_str()) == 0)
    return true;
  if (!IsDirectory(src))
    return Rename(src, dst);

  // src is a directory and dst a non-empty one: move the contents one by one
  const FSTEntry tree = ScanDirectoryTree(src, false);
  bool ok = CreateDir(dst);
  for (const FSTEntry& child : tree.children)
    ok = MoveTree(child.physicalName, dst + DIR_SEP_CHR + child.virtualName) && ok;
  return path_rmdir(src.c_str()) && ok;
}

bool MoveWithOverwrite(std::string_view source_path, std::string_view dest_path)
{
  DEBUG_LOG_FMT(COMMON, "{}: {} --> {}", __func__, source_path, dest_path);
  if (!MoveTree(std::string(source_path), std::string(dest_path)))
    ERROR_LOG_FMT(COMMON, "{}: failed {} --> {}", __func__, source_path, dest_path);
  return true;
}

std::string GetTempFilenameForAtomicWrite(std::string path)
{
  return std::move(path) + ".xxx";
}

const std::string& GetSysDirectory()
{
#ifdef ANDROID
  static const std::string sys_directory = s_android_sys_directory + DIR_SEP;
  ASSERT_MSG(COMMON, !s_android_sys_directory.empty(), "Sys directory has not been set");
#else
  static const std::string sys_directory = s_libretro_sys_directory + DIR_SEP;
#endif
  return sys_directory;
}

#ifndef ANDROID
void SetSysDirectory(const std::string& path)
{
  INFO_LOG_FMT(COMMON, "Setting Sys directory to {}", path);
  s_libretro_sys_directory = path;
}
#endif

#ifdef ANDROID
void SetSysDirectory(const std::string& path)
{
  INFO_LOG_FMT(COMMON, "Setting Sys directory to {}", path);
  ASSERT_MSG(COMMON, s_android_sys_directory.empty(), "Sys directory already set to {}",
             s_android_sys_directory);
  s_android_sys_directory = path;
}
#endif

static std::string s_user_paths[NUM_PATH_INDICES];
static void RebuildUserDirectories(unsigned int dir_index)
{
  switch (dir_index)
  {
  case D_USER_IDX:
    s_user_paths[D_GCUSER_IDX] = s_user_paths[D_USER_IDX] + GC_USER_DIR DIR_SEP;
    s_user_paths[D_WIIROOT_IDX] = s_user_paths[D_USER_IDX] + WII_USER_DIR DIR_SEP;
    s_user_paths[D_TRIUSER_IDX] = s_user_paths[D_USER_IDX] + TRI_USER_DIR DIR_SEP;
    s_user_paths[D_CONFIG_IDX] = s_user_paths[D_USER_IDX] + CONFIG_DIR DIR_SEP;
    s_user_paths[D_GAMESETTINGS_IDX] = s_user_paths[D_USER_IDX] + GAMESETTINGS_DIR DIR_SEP;
    s_user_paths[D_MAPS_IDX] = s_user_paths[D_USER_IDX] + MAPS_DIR DIR_SEP;
    s_user_paths[D_CACHE_IDX] = s_user_paths[D_USER_IDX] + CACHE_DIR DIR_SEP;
    s_user_paths[D_COVERCACHE_IDX] = s_user_paths[D_CACHE_IDX] + COVERCACHE_DIR DIR_SEP;
    s_user_paths[D_REDUMPCACHE_IDX] = s_user_paths[D_CACHE_IDX] + REDUMPCACHE_DIR DIR_SEP;
    s_user_paths[D_SHADERCACHE_IDX] = s_user_paths[D_CACHE_IDX] + SHADERCACHE_DIR DIR_SEP;
    s_user_paths[D_RETROACHIEVEMENTSCACHE_IDX] =
        s_user_paths[D_CACHE_IDX] + RETROACHIEVEMENTSCACHE_DIR DIR_SEP;
    s_user_paths[D_STATESAVES_IDX] = s_user_paths[D_USER_IDX] + STATESAVES_DIR DIR_SEP;
    s_user_paths[D_LOAD_IDX] = s_user_paths[D_USER_IDX] + LOAD_DIR DIR_SEP;
    s_user_paths[D_HIRESTEXTURES_IDX] = s_user_paths[D_LOAD_IDX] + HIRES_TEXTURES_DIR DIR_SEP;
    s_user_paths[D_RIIVOLUTION_IDX] = s_user_paths[D_LOAD_IDX] + RIIVOLUTION_DIR DIR_SEP;
    s_user_paths[D_DUMP_IDX] = s_user_paths[D_USER_IDX] + DUMP_DIR DIR_SEP;
    s_user_paths[D_DUMPOBJECTS_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_OBJECTS_DIR DIR_SEP;
    s_user_paths[D_DUMPAUDIO_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_AUDIO_DIR DIR_SEP;
    s_user_paths[D_DUMPTEXTURES_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_TEXTURES_DIR DIR_SEP;
    s_user_paths[D_DUMPDSP_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_DSP_DIR DIR_SEP;
    s_user_paths[D_DUMPSSL_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_SSL_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_DEBUG_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_BRANCHWATCH_IDX] =
        s_user_paths[D_DUMPDEBUG_IDX] + DUMP_DEBUG_BRANCHWATCH_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_JITBLOCKS_IDX] =
        s_user_paths[D_DUMPDEBUG_IDX] + DUMP_DEBUG_JITBLOCKS_DIR DIR_SEP;
    s_user_paths[D_LOGS_IDX] = s_user_paths[D_USER_IDX] + LOGS_DIR DIR_SEP;
    s_user_paths[D_THEMES_IDX] = s_user_paths[D_USER_IDX] + THEMES_DIR DIR_SEP;
    s_user_paths[D_STYLES_IDX] = s_user_paths[D_USER_IDX] + STYLES_DIR DIR_SEP;
    s_user_paths[D_PIPES_IDX] = s_user_paths[D_USER_IDX] + PIPES_DIR DIR_SEP;
    s_user_paths[D_WFSROOT_IDX] = s_user_paths[D_USER_IDX] + WFSROOT_DIR DIR_SEP;
    s_user_paths[D_BACKUP_IDX] = s_user_paths[D_USER_IDX] + BACKUP_DIR DIR_SEP;
    s_user_paths[D_RESOURCEPACK_IDX] = s_user_paths[D_USER_IDX] + RESOURCEPACK_DIR DIR_SEP;
    s_user_paths[D_DYNAMICINPUT_IDX] = s_user_paths[D_LOAD_IDX] + DYNAMICINPUT_DIR DIR_SEP;
    s_user_paths[D_GRAPHICSMOD_IDX] = s_user_paths[D_LOAD_IDX] + GRAPHICSMOD_DIR DIR_SEP;
    s_user_paths[D_BANNERS_WIIROOT_IDX] = s_user_paths[D_LOAD_IDX] + WIIBANNERS_DIR DIR_SEP;
    s_user_paths[D_FIRMWARE_IDX] = s_user_paths[D_LOAD_IDX] + FIRMWARE_DIR DIR_SEP;
    s_user_paths[D_WIISYSCONF_IDX] = s_user_paths[D_WIIROOT_IDX] + WII_SYSCONF_DIR DIR_SEP;
    s_user_paths[D_WIISDCARDSYNCFOLDER_IDX] = s_user_paths[D_LOAD_IDX] + WIISDSYNC_DIR DIR_SEP;
    s_user_paths[F_DOLPHINCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + DOLPHIN_CONFIG;
    s_user_paths[F_GCPADCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GCPAD_CONFIG;
    s_user_paths[F_WIIPADCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + WIIPAD_CONFIG;
    s_user_paths[F_GCKEYBOARDCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GCKEYBOARD_CONFIG;
    s_user_paths[F_GFXCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GFX_CONFIG;
    s_user_paths[F_LOGGERCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + LOGGER_CONFIG;
    s_user_paths[F_DUALSHOCKUDPCLIENTCONFIG_IDX] =
        s_user_paths[D_CONFIG_IDX] + DUALSHOCKUDPCLIENT_CONFIG;
    s_user_paths[F_FREELOOKCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + FREELOOK_CONFIG;
    s_user_paths[F_RETROACHIEVEMENTSCONFIG_IDX] =
        s_user_paths[D_CONFIG_IDX] + RETROACHIEVEMENTS_CONFIG;
    s_user_paths[F_MAINLOG_IDX] = s_user_paths[D_LOGS_IDX] + MAIN_LOG;
    s_user_paths[F_MEM1DUMP_IDX] = s_user_paths[D_DUMP_IDX] + MEM1_DUMP;
    s_user_paths[F_MEM2DUMP_IDX] = s_user_paths[D_DUMP_IDX] + MEM2_DUMP;
    s_user_paths[F_ARAMDUMP_IDX] = s_user_paths[D_DUMP_IDX] + ARAM_DUMP;
    s_user_paths[F_FAKEVMEMDUMP_IDX] = s_user_paths[D_DUMP_IDX] + FAKEVMEM_DUMP;
    s_user_paths[F_GCSRAM_IDX] = s_user_paths[D_GCUSER_IDX] + GC_SRAM;
    s_user_paths[F_WIISDCARDIMAGE_IDX] = s_user_paths[D_LOAD_IDX] + WII_SD_CARD_IMAGE;
    s_user_paths[F_WIISYSCONF_IDX] = s_user_paths[D_WIISYSCONF_IDX] + WII_SYSCONF;

    s_user_paths[D_MEMORYWATCHER_IDX] = s_user_paths[D_USER_IDX] + MEMORYWATCHER_DIR DIR_SEP;
    s_user_paths[F_MEMORYWATCHERLOCATIONS_IDX] =
        s_user_paths[D_MEMORYWATCHER_IDX] + MEMORYWATCHER_LOCATIONS;
    s_user_paths[F_MEMORYWATCHERSOCKET_IDX] =
        s_user_paths[D_MEMORYWATCHER_IDX] + MEMORYWATCHER_SOCKET;

    s_user_paths[D_GBAUSER_IDX] = s_user_paths[D_USER_IDX] + GBA_USER_DIR DIR_SEP;
    s_user_paths[D_GBASAVES_IDX] = s_user_paths[D_GBAUSER_IDX] + GBASAVES_DIR DIR_SEP;
    s_user_paths[F_GBABIOS_IDX] = s_user_paths[D_GBAUSER_IDX] + GBA_BIOS;

    s_user_paths[D_ASM_ROOT_IDX] = s_user_paths[D_USER_IDX] + ASSEMBLY_DIR DIR_SEP;

    break;

  case D_CONFIG_IDX:
    s_user_paths[F_DOLPHINCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + DOLPHIN_CONFIG;
    s_user_paths[F_GCPADCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GCPAD_CONFIG;
    s_user_paths[F_GCKEYBOARDCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GCKEYBOARD_CONFIG;
    s_user_paths[F_WIIPADCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + WIIPAD_CONFIG;
    s_user_paths[F_GFXCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + GFX_CONFIG;
    s_user_paths[F_LOGGERCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + LOGGER_CONFIG;
    s_user_paths[F_DUALSHOCKUDPCLIENTCONFIG_IDX] =
        s_user_paths[D_CONFIG_IDX] + DUALSHOCKUDPCLIENT_CONFIG;
    s_user_paths[F_FREELOOKCONFIG_IDX] = s_user_paths[D_CONFIG_IDX] + FREELOOK_CONFIG;
    s_user_paths[F_RETROACHIEVEMENTSCONFIG_IDX] =
        s_user_paths[D_CONFIG_IDX] + RETROACHIEVEMENTS_CONFIG;
    break;

  case D_CACHE_IDX:
    s_user_paths[D_COVERCACHE_IDX] = s_user_paths[D_CACHE_IDX] + COVERCACHE_DIR DIR_SEP;
    s_user_paths[D_REDUMPCACHE_IDX] = s_user_paths[D_CACHE_IDX] + REDUMPCACHE_DIR DIR_SEP;
    s_user_paths[D_SHADERCACHE_IDX] = s_user_paths[D_CACHE_IDX] + SHADERCACHE_DIR DIR_SEP;
    s_user_paths[D_RETROACHIEVEMENTSCACHE_IDX] =
        s_user_paths[D_CACHE_IDX] + RETROACHIEVEMENTSCACHE_DIR DIR_SEP;
    break;

  case D_GCUSER_IDX:
    s_user_paths[F_GCSRAM_IDX] = s_user_paths[D_GCUSER_IDX] + GC_SRAM;
    break;

  case D_DUMP_IDX:
    s_user_paths[D_DUMPOBJECTS_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_OBJECTS_DIR DIR_SEP;
    s_user_paths[D_DUMPAUDIO_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_AUDIO_DIR DIR_SEP;
    s_user_paths[D_DUMPTEXTURES_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_TEXTURES_DIR DIR_SEP;
    s_user_paths[D_DUMPDSP_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_DSP_DIR DIR_SEP;
    s_user_paths[D_DUMPSSL_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_SSL_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_IDX] = s_user_paths[D_DUMP_IDX] + DUMP_DEBUG_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_BRANCHWATCH_IDX] =
        s_user_paths[D_DUMPDEBUG_IDX] + DUMP_DEBUG_BRANCHWATCH_DIR DIR_SEP;
    s_user_paths[D_DUMPDEBUG_JITBLOCKS_IDX] =
        s_user_paths[D_DUMPDEBUG_IDX] + DUMP_DEBUG_JITBLOCKS_DIR DIR_SEP;
    s_user_paths[F_MEM1DUMP_IDX] = s_user_paths[D_DUMP_IDX] + MEM1_DUMP;
    s_user_paths[F_MEM2DUMP_IDX] = s_user_paths[D_DUMP_IDX] + MEM2_DUMP;
    s_user_paths[F_ARAMDUMP_IDX] = s_user_paths[D_DUMP_IDX] + ARAM_DUMP;
    s_user_paths[F_FAKEVMEMDUMP_IDX] = s_user_paths[D_DUMP_IDX] + FAKEVMEM_DUMP;
    break;

  case D_LOGS_IDX:
    s_user_paths[F_MAINLOG_IDX] = s_user_paths[D_LOGS_IDX] + MAIN_LOG;
    break;

  case D_LOAD_IDX:
    s_user_paths[D_HIRESTEXTURES_IDX] = s_user_paths[D_LOAD_IDX] + HIRES_TEXTURES_DIR DIR_SEP;
    s_user_paths[D_RIIVOLUTION_IDX] = s_user_paths[D_LOAD_IDX] + RIIVOLUTION_DIR DIR_SEP;
    s_user_paths[D_DYNAMICINPUT_IDX] = s_user_paths[D_LOAD_IDX] + DYNAMICINPUT_DIR DIR_SEP;
    s_user_paths[D_GRAPHICSMOD_IDX] = s_user_paths[D_LOAD_IDX] + GRAPHICSMOD_DIR DIR_SEP;
    s_user_paths[D_BANNERS_WIIROOT_IDX] = s_user_paths[D_LOAD_IDX] + WIIBANNERS_DIR DIR_SEP;
    break;
  }
}

// Gets a set user directory path
// Don't call prior to setting the base user directory
const std::string& GetUserPath(unsigned int dir_index)
{
  return s_user_paths[dir_index];
}

// Sets a user directory path
// Rebuilds internal directory structure to compensate for the new directory
void SetUserPath(unsigned int dir_index, std::string path)
{
  if (path.empty())
    return;

#ifdef _WIN32
  // On Windows, replace all '\' with '/' since we assume the latter in various places in the
  // codebase.
  for (char& c : path)
  {
    if (c == '\\')
      c = '/';
  }
#endif

  // Directories should end with a separator, files should not.
  while (path.ends_with('/'))
    path.pop_back();
  if (path.empty())
    return;
  const bool is_directory = dir_index < FIRST_FILE_USER_PATH_IDX;
  if (is_directory)
    path.push_back('/');

  s_user_paths[dir_index] = std::move(path);
  RebuildUserDirectories(dir_index);
}

std::string JoinPath(const std::string& directory, const std::string& path)
{
  std::string joined;
  if (directory.empty() || path_is_absolute(path.c_str()))
    return path;
  joined = directory;
  if (joined.back() != DIR_SEP_CHR && joined.back() != '\\')
    joined += DIR_SEP_CHR;
  return joined + path;
}

LineReader::LineReader(const std::string& filename) : m_file(filename, "rb")
{
  u64 size;
  if (!m_file)
    return;
  m_map = reinterpret_cast<const char*>(m_file.GetMappedPtr(&size));
  if (m_map)
  {
    m_size = size_t(size);
    return;
  }
  m_window_size = 64 * 1024;
  m_window = static_cast<char*>(std::malloc(m_window_size));
  if (!m_window)
    m_file.Close();
}

LineReader::~LineReader()
{
  std::free(m_window);
}

bool LineReader::Next(const char** text, size_t* length)
{
  const char* base;
  const char* nl;
  size_t len;

  if (!m_file.IsOpen())
    return false;

  for (;;)
  {
    base = m_map ? m_map : m_window;
    nl = static_cast<const char*>(std::memchr(base + m_pos, '\n', m_size - m_pos));
    if (nl || m_map || m_eof)
      break;

    // keep the partial line, then fill behind it; the window grows only
    // for a line longer than it
    std::memmove(m_window, m_window + m_pos, m_size - m_pos);
    m_size -= m_pos;
    m_pos = 0;
    if (m_size == m_window_size)
    {
      char* const grown = static_cast<char*>(std::realloc(m_window, m_window_size * 2));
      if (!grown)
      {
        m_eof = true;
        continue;
      }
      m_window = grown;
      m_window_size *= 2;
    }
    {
      size_t got = 0;
      m_file.ReadArray(m_window + m_size, m_window_size - m_size, &got);
      m_file.ClearError();
      if (!got)
        m_eof = true;
      m_size += got;
    }
  }

  if (nl)
    len = size_t(nl - (base + m_pos));
  else if (m_pos < m_size)
    len = m_size - m_pos;
  else
    return false;

  *text = base + m_pos;
  m_pos += len + (nl ? 1 : 0);
  if (len && (*text)[len - 1] == '\r')
    len--;
  *length = len;
  return true;
}

bool ForEachLine(const std::string& filename, void (*line)(void* ud, const char* text, size_t length),
                 void* ud)
{
  LineReader reader(filename);
  const char* text;
  size_t length;

  if (!reader.IsOpen())
    return false;
  while (reader.Next(&text, &length))
    line(ud, text, length);
  return true;
}

bool WriteStringToFile(const std::string& filename, std::string_view str)
{
  return filestream_write_file(filename.c_str(), str.data(), s64(str.size()));
}

bool ReadFileToString(const std::string& filename, std::string& str)
{
  File::IOFile file(filename, "rb");
  u64 size;
  const u8* map;

  if (!file)
    return false;
  map = file.GetMappedPtr(&size);
  if (map)
  {
    str.assign(reinterpret_cast<const char*>(map), size);
    return true;
  }
  str.resize(file.GetSize());
  return file.ReadArray(str.data(), str.size());
}

}  // namespace File
