// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace DiscIO
{
class FileInfo;
struct Partition;
class Volume;

u64 ReadFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
             u8* buffer, u64 max_buffer_size, u64 offset_in_file = 0);
}  // namespace DiscIO
