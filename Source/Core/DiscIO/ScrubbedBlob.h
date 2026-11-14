// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "DiscIO/Blob.h"
#include "DiscIO/DiscScrubber.h"

namespace DiscIO
{
// This class wraps another BlobReader and zeroes out data that has been
// identified by DiscScrubber as unused.
class ScrubbedBlob final : public BlobReaderOfKind<BlobReaderKind::Scrubbed>
{
public:
  static ScrubbedBlob* Create(const std::string& path);
  ~ScrubbedBlob();

  BlobType GetBlobType() const { return m_blob_reader->GetBlobType(); }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_blob_reader->GetRawSize(); }
  u64 GetDataSize() const { return m_blob_reader->GetDataSize(); }
  DataSizeType GetDataSizeType() const { return m_blob_reader->GetDataSizeType(); }

  u64 GetBlockSize() const { return m_blob_reader->GetBlockSize(); }
  bool HasFastRandomAccessInBlock() const { return m_blob_reader->HasFastRandomAccessInBlock(); }
  std::string GetCompressionMethod() const { return m_blob_reader->GetCompressionMethod(); }
  std::optional<int> GetCompressionLevel() const { return m_blob_reader->GetCompressionLevel(); }

  bool Read(u64 offset, u64 size, u8* out_ptr);

private:
  // takes blob_reader
  ScrubbedBlob(BlobReader* blob_reader, DiscScrubber scrubber);

  BlobReader* m_blob_reader;  // owned
  DiscScrubber m_scrubber;
};

}  // namespace DiscIO
