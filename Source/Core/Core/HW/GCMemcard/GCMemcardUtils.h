// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/HW/GCMemcard/GCMemcard.h"

namespace Memcard
{
bool HasSameIdentity(const DEntry& lhs, const DEntry& rhs);

// Writes a GameCube memory card savefile to a file in the GCI format.
bool WriteGCISavefile(const std::string& filename, const Savefile& savefile);

// Generates a filename (without extension) for the given directory entry.
std::string GenerateFilename(const DEntry& entry);
}  // namespace Memcard
