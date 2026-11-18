// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/DiscExtractor.h"

#include <cinttypes>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
u64 ReadFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
             u8* buffer, u64 max_buffer_size, u64 offset_in_file)
{
  if (!file_info || file_info->IsDirectory() || offset_in_file >= file_info->GetSize())
    return 0;

  const u64 file_left = file_info->GetSize() - offset_in_file;
  const u64 read_length = max_buffer_size < file_left ? max_buffer_size : file_left;

  DEBUG_LOG(DISCIO,
            "Reading %" PRIx64 " bytes at %" PRIx64 " from file %s. Offset: %" PRIx64 " Size: %x",
            read_length, offset_in_file, file_info->GetPath().c_str(), file_info->GetOffset(),
            file_info->GetSize());

  if (!volume.Read(file_info->GetOffset() + offset_in_file, read_length, buffer, partition))
    return 0;

  return read_length;
}

}  // namespace DiscIO
