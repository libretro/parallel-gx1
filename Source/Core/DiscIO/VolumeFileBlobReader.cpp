// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeFileBlobReader.h"

#include <memory>
#include <string_view>

#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
VolumeFileBlobReader* VolumeFileBlobReader::Create(const Volume& volume, const Partition& partition,
                                                   std::string_view file_path)
{
  const FileSystem* file_system = volume.GetFileSystem(partition);
  if (!file_system)
    return nullptr;

  FileInfo file_info;
  if (!file_system->FindFileInfo(file_path, &file_info) || file_info.IsDirectory())
    return nullptr;

  return new VolumeFileBlobReader(volume, partition, file_info);
}

VolumeFileBlobReader::VolumeFileBlobReader(const Volume& volume, const Partition& partition,
                                           const FileInfo& file_info)
    : m_volume(volume), m_partition(partition), m_file_info(file_info)
{
}

VolumeFileBlobReader::~VolumeFileBlobReader() = default;

BlobReader* VolumeFileBlobReader::CopyReader() const
{
  ASSERT_MSG(DISCIO, false, "Unimplemented");
  return nullptr;
}

u64 VolumeFileBlobReader::GetDataSize() const
{
  return m_file_info.GetSize();
}

u64 VolumeFileBlobReader::GetRawSize() const
{
  return GetDataSize();
}

u64 VolumeFileBlobReader::GetBlockSize() const
{
  return m_volume.GetBlobReader().GetBlockSize();
}

bool VolumeFileBlobReader::HasFastRandomAccessInBlock() const
{
  return m_volume.GetBlobReader().HasFastRandomAccessInBlock();
}

std::string VolumeFileBlobReader::GetCompressionMethod() const
{
  return m_volume.GetBlobReader().GetCompressionMethod();
}

std::optional<int> VolumeFileBlobReader::GetCompressionLevel() const
{
  return m_volume.GetBlobReader().GetCompressionLevel();
}

bool VolumeFileBlobReader::Read(u64 offset, u64 length, u8* out_ptr)
{
  if (offset + length > m_file_info.GetSize())
    return false;

  return m_volume.Read(m_file_info.GetOffset() + offset, length, out_ptr, m_partition);
}
}  // namespace DiscIO
