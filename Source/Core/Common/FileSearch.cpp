// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FileSearch.h"

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

static bool MatchesAny(const char* name, size_t name_len, const std::string_view* exts,
                       size_t ext_count)
{
  size_t i;
  for (i = 0; i < ext_count; i++)
    if (EndsWithNoCase(name, name_len, exts[i]))
      return true;
  return false;
}

static void Search(const std::string& directory, const std::string_view* exts, size_t ext_count,
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

    if (!ext_count || (!is_dir && MatchesAny(name, name_len, exts, ext_count)))
      result->push_back(path);
    if (is_dir && recursive)
      Search(path, exts, ext_count, true, result);
  }
  retro_closedir(dir);
}

static void SiftDown(std::string* paths, size_t root, size_t count)
{
  for (;;)
  {
    size_t child = root * 2 + 1;
    if (child >= count)
      return;
    if (child + 1 < count && paths[child] < paths[child + 1])
      child++;
    if (!(paths[root] < paths[child]))
      return;
    paths[root].swap(paths[child]);
    root = child;
  }
}

std::vector<std::string> DoFileSearch(const std::string_view* directories, size_t directory_count,
                                      const std::string_view* exts, size_t ext_count,
                                      bool recursive)
{
  std::vector<std::string> result;
  size_t i;
  for (i = 0; i < directory_count; i++)
  {
    std::string dir(directories[i]);
    while (dir.size() > 1 && (dir.back() == DIR_SEP_CHR || dir.back() == '\\'))
      dir.pop_back();
    Search(dir, exts, ext_count, recursive, &result);
  }

  // Sort in place (heap sort) and drop duplicates (the caller gave e.g.
  // duplicate or overlapping directories).
  const size_t count = result.size();
  if (count > 1)
  {
    for (i = count / 2; i > 0; i--)
      SiftDown(result.data(), i - 1, count);
    for (i = count - 1; i > 0; i--)
    {
      result[0].swap(result[i]);
      SiftDown(result.data(), 0, i);
    }
    size_t kept = 1;
    for (i = 1; i < count; i++)
    {
      if (result[i] != result[kept - 1])
      {
        if (i != kept)
          result[kept].swap(result[i]);
        kept++;
      }
    }
    result.resize(kept);
  }

  // Dolphin expects to be able to use "/" (DIR_SEP) everywhere.
#ifdef _WIN32
  for (auto& path : result)
  {
    for (char& c : path)
    {
      if (c == '\\')
        c = DIR_SEP_CHR;
    }
  }
#endif

  return result;
}

}  // namespace Common
