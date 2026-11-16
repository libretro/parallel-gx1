// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheatCodes.h"

#include <cstdio>
#include <cstring>

#include "Common/IniData.h"
#include "Common/IniFile.h"

static void ReadEnabledOrDisabled(const Common::IniFile& ini, const char* section, bool enabled,
                                  char* codes, size_t count, size_t stride, size_t name_offset,
                                  size_t enabled_offset)
{
  const Common::IniFile::Section* const lines = ini.GetSection(section);
  size_t i;
  if (!lines)
    return;
  for (i = 0; i < lines->Data()->num_lines; i++)
  {
    const char* text;
    size_t len;
    size_t c;
    if (!ini_section_line(lines->Data(), i, false, &text, &len) || !len || text[0] != '$')
      continue;
    for (c = 0; c < count; c++)
    {
      char* const code = codes + c * stride;
      const char* name;
      std::memcpy(&name, code + name_offset, sizeof(name));
      // Exclude the initial '$' from the comparison.
      if (name && std::strlen(name) == len - 1 && !std::memcmp(name, text + 1, len - 1))
        std::memcpy(code + enabled_offset, &enabled, sizeof(enabled));
    }
  }
}

void ReadEnabledAndDisabled(const Common::IniFile& ini, const char* section, void* codes,
                            size_t count, size_t stride, size_t name_offset, size_t enabled_offset)
{
  char name[64];
  if (std::snprintf(name, sizeof(name), "%s_Enabled", section) < int(sizeof(name)))
  {
    ReadEnabledOrDisabled(ini, name, true, static_cast<char*>(codes), count, stride, name_offset,
                          enabled_offset);
  }
  if (std::snprintf(name, sizeof(name), "%s_Disabled", section) < int(sizeof(name)))
  {
    ReadEnabledOrDisabled(ini, name, false, static_cast<char*>(codes), count, stride, name_offset,
                          enabled_offset);
  }
}
