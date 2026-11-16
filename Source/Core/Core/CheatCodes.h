// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Common/IniFile.h"

// The codes a game's INI file names in section_Enabled are turned on, then those it names in
// section_Disabled are turned off; a code is named by a "$name" line. The codes are count structs
// of stride bytes, each holding its name (a char*, which may be null) at name_offset and whether it
// is on (a bool) at enabled_offset.
void ReadEnabledAndDisabled(const Common::IniFile& ini, const char* section, void* codes,
                            size_t count, size_t stride, size_t name_offset, size_t enabled_offset);

// The same for the codes of a vector, each having a std::string name and a bool enabled.
template <typename T>
void ReadEnabledOrDisabled(const Common::IniFile& ini, const std::string& section, bool enabled,
                           std::vector<T>* codes)
{
  std::vector<std::string> lines;
  ini.GetLines(section, &lines, false);

  for (const std::string& line : lines)
  {
    if (line.empty() || line[0] != '$')
      continue;

    for (T& code : *codes)
    {
      // Exclude the initial '$' from the comparison.
      if (line.compare(1, std::string::npos, code.name) == 0)
        code.enabled = enabled;
    }
  }
}

template <typename T>
void ReadEnabledAndDisabled(const Common::IniFile& ini, const std::string& section,
                            std::vector<T>* codes)
{
  ReadEnabledOrDisabled(ini, section + "_Enabled", true, codes);
  ReadEnabledOrDisabled(ini, section + "_Disabled", false, codes);
}
