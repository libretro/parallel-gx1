// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"

namespace Common
{
enum class FromWhichRoot
{
  None,        // a path relative to the NAND root
  Configured,  // not related to currently running game - use D_WIIROOT_IDX
  Session,     // request from currently running game - use D_SESSION_WIIROOT_IDX
  Banners,     // fallback for Wii savegame banners - use D_BANNERS_WIIROOT_IDX
};

std::string RootUserPath(FromWhichRoot from);

// The following functions return paths relative to the NAND root.
// Unless from is None, the NAND root on the host filesystem will be prepended to the path.
// TODO: remove the from parameter after all code is migrated off direct FS access.

// Returns /import/%08x/%08x. Intended for use by ES.
std::string GetImportTitlePath(u64 title_id, FromWhichRoot from = FromWhichRoot::None);

std::string GetTicketFileName(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetV1TicketFileName(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetTitlePath(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetTitleDataPath(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetTitleContentPath(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetTMDFileName(u64 title_id, FromWhichRoot from = FromWhichRoot::None);
std::string GetMiiDatabasePath(FromWhichRoot from = FromWhichRoot::None);

// Escapes characters that are invalid or have special meanings in the host file system
std::string EscapeFileName(const std::string& filename);
// Escapes characters that are invalid or have special meanings in the host file system
std::string EscapePath(const std::string& path);
// Writes EscapePath(path) to out; returns false, leaving out unspecified, when it does not fit
// in out_size bytes with its terminator.
bool EscapePathTo(const char* path, char* out, size_t out_size);
// Writes EscapeFileName of the size bytes at name to out, always terminated; returns false when
// it had to be cut short to fit in out_size bytes.
bool EscapeFileNameTo(const char* name, size_t size, char* out, size_t out_size);
// Reverses escaping done by EscapeFileName
std::string UnescapeFileName(const std::string& filename);
}  // namespace Common
