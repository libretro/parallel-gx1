// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common
{
class IniFile;
}

namespace ActionReplay
{
struct AREntry
{
  u32 cmd_addr;
  u32 value;
};

/* An Action Replay code of the game's INI files: its name and its decrypted lines. */
struct ARCode
{
  char* name;
  AREntry* ops;
  u32 num_ops;
  u32 cap_ops;
  bool enabled;
};

/* The codes of the game's INI files, each with its own allocations. */
struct ARCodeList
{
  ARCode* codes;
  u32 count;
  u32 cap;
};

void FreeCodeList(ARCodeList* list);
/* One more line behind the code's; dropped when out of memory. */
void CodeAddOp(ARCode* code, u32 cmd_addr, u32 value);

void RunAllActive();

void ApplyCodes(const ARCode* codes, size_t count);
size_t CountEnabledCodes();
void LoadAndApplyCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini);

// The codes of both files into list, whose earlier codes are freed.
void LoadCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini,
               ARCodeList* list);
}  // namespace ActionReplay
