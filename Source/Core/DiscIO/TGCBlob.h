// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
static constexpr u32 TGC_MAGIC = 0xA2380FAE;

struct TGCHeader
{
  u32 magic;
  u32 unknown_1;
  u32 tgc_header_size;
  u32 disc_header_area_size;

  u32 fst_real_offset;
  u32 fst_size;
  u32 fst_max_size;
  u32 dol_real_offset;

  u32 dol_size;
  u32 file_area_real_offset;
  u32 unknown_2;
  u32 unknown_3;

  u32 unknown_4;
  u32 file_area_virtual_offset;
};

class TGCFileReader final : public BlobReaderOfKind<BlobReaderKind::TGC>
{
public:
  static TGCFileReader* Create(File::DirectIOFile file);

  BlobType GetBlobType() const { return BlobType::TGC; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_size; }
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const { return 0; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);

private:
  TGCFileReader(File::DirectIOFile file);

  File::DirectIOFile m_file;
  u64 m_size;

  std::vector<u8> m_fst;

  // Stored as big endian in memory, regardless of the host endianness
  TGCHeader m_header = {};
};

}  // namespace DiscIO
