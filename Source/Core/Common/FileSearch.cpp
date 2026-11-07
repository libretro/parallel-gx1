// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FileSearch.h"

#include <algorithm>
#include <cstring>

#include <retro_dirent.h>

#include "Common/CommonPaths.h"

namespace Common
{
static bool EndsWithNoCase(const char* name, size_t name_len, std::string_view ext)
{
  size_t i;
  if (name_len < ext.size())
    return false;
  name += name_len - ext.size();
  for (i = 0; i < ext.size(); i++)
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

static bool MatchesAny(const char* name, size_t name_len, std::span<const std::string_view> exts)
{
  size_t i;
  for (i = 0; i < exts.size(); i++)
    if (EndsWithNoCase(name, name_len, exts[i]))
      return true;
  return false;
}

static void Search(const std::string& directory, std::span<const std::string_view> exts,
                   bool recursive, std::vector<std::string>* result)
{
  struct RDIR* dir = retro_opendir_include_hidden(directory.c_str(), true);
  if (!dir)
    return;
  while (retro_readdir(dir))
  {
    const char* name = retro_dirent_get_name(dir);
    size_t name_len;
    bool is_dir;
    if (!name || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
      continue;
    name_len = std::strlen(name);
    is_dir = retro_dirent_is_dir(dir, nullptr);

    std::string path = directory;
    if (!path.empty() && path.back() != DIR_SEP_CHR && path.back() != '\\')
      path += DIR_SEP_CHR;
    path.append(name, name_len);

    if (exts.empty() || (!is_dir && MatchesAny(name, name_len, exts)))
      result->push_back(path);
    if (is_dir && recursive)
      Search(path, exts, true, result);
  }
  retro_closedir(dir);
}

std::vector<std::string> DoFileSearch(std::span<const std::string_view> directories,
                                      std::span<const std::string_view> exts, bool recursive)
{
  std::vector<std::string> result;
  for (const auto& directory : directories)
  {
    std::string dir(directory);
    while (dir.size() > 1 && (dir.back() == DIR_SEP_CHR || dir.back() == '\\'))
      dir.pop_back();
    Search(dir, exts, recursive, &result);
  }

  // Remove duplicates (the caller gave e.g. duplicate or overlapping directories).
  std::ranges::sort(result);
  const auto unique_result = std::ranges::unique(result);
  result.erase(unique_result.begin(), unique_result.end());

  // Dolphin expects to be able to use "/" (DIR_SEP) everywhere.
#ifdef _WIN32
  for (auto& path : result)
    std::ranges::replace(path, '\\', DIR_SEP_CHR);
#endif

  return result;
}

}  // namespace Common
