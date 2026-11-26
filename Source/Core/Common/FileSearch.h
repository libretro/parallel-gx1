// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/PathList.h"

namespace Common
{
// Adds the paths found in the directories to result, then sorts it and drops repeats; the paths
// have "/" as their separator. Callers can pass no "exts" to indicate they want all files and
// directories; otherwise, only files matching the extensions (ignoring case) are added.
void DoFileSearch(const char* const* directories, size_t directory_count, const char* const* exts,
                  size_t ext_count, bool recursive, path_list_t* result);

inline void DoFileSearch(const char* directory, const char* ext, bool recursive,
                         path_list_t* result)
{
  DoFileSearch(&directory, 1, &ext, 1, recursive, result);
}

}  // namespace Common
