// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FileSearch.h"

#include <cstdlib>
#include <cstring>

#include <retro_dirent.h>
#include <retro_miscellaneous.h>

#include "Common/CommonPaths.h"

namespace Common
{
static bool EndsWithNoCase(const char* name, size_t name_len, const char* ext)
{
  const size_t ext_len = std::strlen(ext);
  size_t i;
  if (name_len < ext_len)
    return false;
  name += name_len - ext_len;
  for (i = 0; i < ext_len; i++)
  {
    char a = name[i];
    char b = ext[i];
    if (a >= 'A' && a <= 'Z')
      a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
      b += 'a' - 'A';
    if (a != b)
      return false;
  }
  return true;
}

static bool MatchesAny(const char* name, size_t name_len, const char* const* exts, size_t ext_count)
{
  size_t i;
  for (i = 0; i < ext_count; i++)
    if (EndsWithNoCase(name, name_len, exts[i]))
      return true;
  return false;
}

// Searches the directory whose path is the length bytes in path, a buffer of PATH_MAX_LENGTH that
// its entries' paths are built in; a path that does not fit is skipped.
static void Search(char* path, size_t length, const char* const* exts, size_t ext_count,
                   bool recursive, path_list_t* result)
{
  struct RDIR* dir;
  size_t base = length;
  path[length] = '\0';
  dir = retro_opendir_include_hidden(path, true);
  if (!dir)
    return;
  if (base && path[base - 1] != DIR_SEP_CHR && path[base - 1] != '\\')
  {
    if (base + 1 >= PATH_MAX_LENGTH)
    {
      retro_closedir(dir);
      return;
    }
    path[base++] = DIR_SEP_CHR;
  }
  while (retro_readdir(dir))
  {
    const char* name = retro_dirent_get_name(dir);
    size_t name_len;
    bool is_dir;
    if (!name || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
      continue;
    name_len = std::strlen(name);
    if (base + name_len >= PATH_MAX_LENGTH)
      continue;
    is_dir = retro_dirent_is_dir(dir, nullptr);
    std::memcpy(path + base, name, name_len + 1);

    if (!ext_count || (!is_dir && MatchesAny(name, name_len, exts, ext_count)))
      path_list_add_len(result, path, base + name_len);
    if (is_dir && recursive)
      Search(path, base + name_len, exts, ext_count, true, result);
  }
  retro_closedir(dir);
}

void DoFileSearch(const char* const* directories, size_t directory_count, const char* const* exts,
                  size_t ext_count, bool recursive, path_list_t* result)
{
  char* const path = static_cast<char*>(std::malloc(PATH_MAX_LENGTH));
  size_t i;
  if (path)
  {
    for (i = 0; i < directory_count; i++)
    {
      size_t length = std::strlen(directories[i]);
      while (length > 1 &&
             (directories[i][length - 1] == DIR_SEP_CHR || directories[i][length - 1] == '\\'))
      {
        length--;
      }
      if (length >= PATH_MAX_LENGTH)
        continue;
      std::memcpy(path, directories[i], length);
      Search(path, length, exts, ext_count, recursive, result);
    }
    std::free(path);
  }

  // Sorted, without repeats (the caller gave e.g. duplicate or overlapping directories).
  path_list_sort_unique(result);

  // Dolphin expects to be able to use "/" (DIR_SEP) everywhere.
#ifdef _WIN32
  for (i = 0; i < result->text_size; i++)
  {
    if (result->text[i] == '\\')
      result->text[i] = DIR_SEP_CHR;
  }
#endif
}

}  // namespace Common
