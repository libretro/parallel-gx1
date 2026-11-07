// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Common
{
// Callers can pass empty "exts" to indicate they want all files + directories in results
// Otherwise, only files matching the extensions are returned
std::vector<std::string> DoFileSearch(const std::string_view* directories, size_t directory_count,
                                      const std::string_view* exts, size_t ext_count,
                                      bool recursive = false);

inline std::vector<std::string> DoFileSearch(std::string_view directory, std::string_view ext,
                                             bool recursive = false)
{
  return DoFileSearch(&directory, 1, &ext, 1, recursive);
}

// all files and directories
inline std::vector<std::string> DoFileSearchAll(std::string_view directory, bool recursive = false)
{
  return DoFileSearch(&directory, 1, nullptr, 0, recursive);
}

}  // namespace Common
