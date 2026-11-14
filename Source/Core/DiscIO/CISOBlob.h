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
static constexpr u32 CISO_MAGIC = 0x4F534943;  // "CISO" (byteswapped to little endian)

static constexpr u32 CISO_HEADER_SIZE = 0x8000;
static constexpr u32 CISO_MAP_SIZE = CISO_HEADER_SIZE - sizeof(u32) - sizeof(char) * 4;

struct CISOHeader
{
  // "CISO"
  u32 magic;

  // little endian
  u32 block_size;

  // 0=unused, 1=used, others=invalid
  u8 map[CISO_MAP_SIZE];
};

class CISOFileReader final : public BlobReaderOfKind<BlobReaderKind::CISO>
{
public:
  static CISOFileReader* Create(File::DirectIOFile file);

  BlobType GetBlobType() const { return BlobType::CISO; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::UpperBound; }

  u64 GetBlockSize() const { return m_block_size; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);

private:
  CISOFileReader(File::DirectIOFile file);

  typedef u16 MapType;
  static constexpr MapType UNUSED_BLOCK_ID = UINT16_MAX;

  File::DirectIOFile m_file;
  u64 m_size;
  u32 m_block_size;
  MapType m_ciso_map[CISO_MAP_SIZE];
};

}  // namespace DiscIO
