// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
class PlainFileReader final : public BlobReaderOfKind<BlobReaderKind::Plain>
{
public:
  static PlainFileReader* Create(File::DirectIOFile file);

  BlobType GetBlobType() const { return BlobType::PLAIN; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_size; }
  u64 GetDataSize() const { return m_size; }
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const { return 0; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);
  const u8* View(u64 offset, u64 size) const { return m_file.View(offset, size); }
  void Prefetch(u64 offset, u64 size) const { m_file.Prefetch(offset, size); }

private:
  PlainFileReader(File::DirectIOFile file);

  File::DirectIOFile m_file;
  u64 m_size;
};

}  // namespace DiscIO
