// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

namespace Common
{
class IniFile;
}

// The codes a game's INI file names in section_Enabled are turned on, then those it names in
// section_Disabled are turned off; a code is named by a "$name" line. The codes are count structs
// of stride bytes, each holding its name (a char*, which may be null) at name_offset and whether it
// is on (a bool) at enabled_offset.
void ReadEnabledAndDisabled(const Common::IniFile& ini, const char* section, void* codes,
                            size_t count, size_t stride, size_t name_offset, size_t enabled_offset);
