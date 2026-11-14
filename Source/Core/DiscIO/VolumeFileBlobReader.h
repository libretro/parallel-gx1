// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Filesystem.h"

namespace DiscIO
{
struct Partition;
class Volume;

class VolumeFileBlobReader final : public BlobReaderOfKind<BlobReaderKind::VolumeFile>
{
public:
  static VolumeFileBlobReader* Create(const Volume& volume, const Partition& partition,
                                      std::string_view file_path);
  ~VolumeFileBlobReader();

  BlobType GetBlobType() const { return BlobType::PLAIN; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const;
  bool HasFastRandomAccessInBlock() const;
  std::string GetCompressionMethod() const;
  std::optional<int> GetCompressionLevel() const;

  bool Read(u64 offset, u64 length, u8* out_ptr);

private:
  VolumeFileBlobReader(const Volume& volume, const Partition& partition, const FileInfo& file_info);

  const Volume& m_volume;
  const Partition& m_partition;
  FileInfo m_file_info;
};
}  // namespace DiscIO
