// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Gecko
{
/* A line of a Gecko code: its two words, both 0 when the line is not two hexadecimal numbers,
 * and the line as written. */
struct GeckoCodeLine
{
  u32 address;
  u32 data;
  char* original_line;
};

/* A Gecko code of the game's INI files: its name and its lines. */
struct GeckoCode
{
  char* name;
  GeckoCodeLine* lines;
  u32 num_lines;
  u32 cap_lines;
  bool enabled;
};

/* The codes of the game's INI files, each with its own allocations. */
struct GeckoCodeList
{
  GeckoCode* codes;
  u32 count;
  u32 cap;
};

void FreeCodeList(GeckoCodeList* list);

// Installation address for codehandler.bin in the Game's RAM
constexpr u32 INSTALLER_BASE_ADDRESS = 0x80001800;
constexpr u32 INSTALLER_END_ADDRESS = 0x80003000;
constexpr u32 ENTRY_POINT = INSTALLER_BASE_ADDRESS + 0xA8;
// If the GCT is max-length then this is the second word of the End code (0xF0000000 0x00000000)
// If the table is shorter than the max-length then this address is unused / contains trash.
constexpr u32 HLE_TRAMPOLINE_ADDRESS = INSTALLER_END_ADDRESS - 4;

// This forms part of a communication protocol with HLE_Misc::GeckoCodeHandlerICacheFlush.
// Basically, codehandleronly.s doesn't use ICBI like it's supposed to when patching the
// game's code. This results in the JIT happily ignoring all code patches for blocks that
// are already compiled. The hack for getting around that is that the first 5 frames after
// the handler is installed (0xD01F1BAD -> +5 -> 0xD01F1BB2) cause full ICache resets.
//
// GeckoCodeHandlerICacheFlush will increment this value 5 times then cease flushing the ICache to
// preserve the emulation performance.
constexpr u32 MAGIC_GAMEID = 0xD01F1BAD;

size_t CountEnabledCodes();
void SetActiveCodes(const GeckoCode* gcodes, size_t count);
// Whether the code handler runs in the game; without it, Gecko codes do not apply
void SetCodeHandlerEnabled(bool enabled);
void RunCodeHandler();
void Shutdown();
void DoState(PointerWrap&);

}  // namespace Gecko
