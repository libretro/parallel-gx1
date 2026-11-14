// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "DiscIO/Blob.h"

namespace DiscIO
{

class CacheFiller;

// Reads through a cache filled from a copy of the reader it is made over, on a thread of its own
class CachedBlobReader final : public BlobReaderOfKind<BlobReaderKind::Cached>
{
public:
  // take reader
  CachedBlobReader(BlobReader* reader, bool attempt_to_scrub);
  CachedBlobReader(std::shared_ptr<CacheFiller> cache_filler, BlobReader* reader);
  ~CachedBlobReader();

  BlobReader* CopyReader() const;

  BlobType GetBlobType() const;
  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;

  u64 GetBlockSize() const { return 0; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr);

  bool IsCached() const { return true; }

private:
  // A shared object does the cache filling for sensible CopyReader behavior.
  const std::shared_ptr<CacheFiller> m_cache_filler;

  BlobReader* const m_reader;  // owned
};

// Take reader; the reader returned is the caller's (reader itself if it is cached already)
BlobReader* CreateCachedBlobReader(BlobReader* reader);
BlobReader* CreateScrubbingCachedBlobReader(BlobReader* reader);

}  // namespace DiscIO
