// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"

namespace Common
{
// Large enough for every NAND path built below, terminator included.
constexpr size_t NAND_PATH_SIZE = 64;

// The following functions write paths relative to the NAND root, cut short to fit in out_size.

// Writes /import/%08x/%08x. Intended for use by ES.
void GetImportTitlePath(u64 title_id, char* out, size_t out_size);

void GetTicketFileName(u64 title_id, char* out, size_t out_size);
void GetV1TicketFileName(u64 title_id, char* out, size_t out_size);
void GetTitlePath(u64 title_id, char* out, size_t out_size);
void GetTitleDataPath(u64 title_id, char* out, size_t out_size);
// Writes the path of file_name inside the title's data directory.
void GetTitleDataFilePath(u64 title_id, const char* file_name, char* out, size_t out_size);
void GetTitleContentPath(u64 title_id, char* out, size_t out_size);
// Writes the path of file_name inside the title's content directory.
void GetTitleContentFilePath(u64 title_id, const char* file_name, char* out, size_t out_size);
void GetTMDFileName(u64 title_id, char* out, size_t out_size);

constexpr char MII_DATABASE_PATH[] = "/shared2/menu/FaceLib/RFL_DB.dat";

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
// Reverses escaping done by EscapeFileName on the length bytes at name, in place; returns the
// new length and terminates the result.
size_t UnescapeFileName(char* name, size_t length);
}  // namespace Common
