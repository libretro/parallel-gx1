// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
class SplitPlainFileReader final : public BlobReaderOfKind<BlobReaderKind::SplitPlain>
{
public:
  static SplitPlainFileReader* Create(std::string_view first_file_path);

  BlobType GetBlobType() const { return BlobType::SPLIT_PLAIN; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_size; }
  u64 GetDataSize() const { return m_size; }
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const { return 0; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);

private:
  struct SingleFile
  {
    File::DirectIOFile file;
    u64 offset = 0;
    u64 size = 0;
  };

  SplitPlainFileReader() = default;

  static constexpr size_t MAX_FILES = 64;
  SingleFile m_files[MAX_FILES];
  size_t m_file_count = 0;
  u64 m_size;
};

}  // namespace DiscIO
