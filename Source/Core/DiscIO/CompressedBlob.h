// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// WARNING Code not big-endian safe.

// To create new compressed BLOBs, use CompressFileToBlob.

// File format
// * Header
// * [Block Pointers interleaved with block hashes (hash of decompressed data)]
// * [Data]

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
static constexpr u32 GCZ_MAGIC = 0xB10BC001;

// GCZ file structure:
// BlobHeader
// u64 offsetsToBlocks[n], top bit specifies whether the block is compressed, or not.
// compressed data

// Blocks that won't compress to less than 97% of the original size are stored as-is.
struct CompressedBlobHeader  // 32 bytes
{
  u32 magic_cookie;  // 0xB10BB10B
  u32 sub_type;      // GC image, whatever
  u64 compressed_data_size;
  u64 disc_size;
  u32 block_size;
  u32 num_blocks;
};

class CompressedBlobReader final : public SectorReader
{
public:
  static CompressedBlobReader* Create(File::DirectIOFile file, const std::string& filename);
  ~CompressedBlobReader();

  const CompressedBlobHeader& GetHeader() const { return m_header; }

  BlobType GetBlobType() const { return BlobType::GCZ; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_file_size; }
  u64 GetDataSize() const { return m_header.disc_size; }
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const { return m_header.block_size; }
  bool HasFastRandomAccessInBlock() const { return false; }
  std::string GetCompressionMethod() const { return "Deflate"; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  u64 GetBlockCompressedSize(u64 block_num) const;
  bool GetBlock(u64 block_num, u8* out_ptr);

private:
  CompressedBlobReader(File::DirectIOFile file, std::string filename);
  bool Initialize();
  bool ValidateBlockPointers() const;

  CompressedBlobHeader m_header = {};
  std::vector<u64> m_block_pointers = {};
  std::vector<u32> m_hashes = {};
  u64 m_data_offset = 0;
  File::DirectIOFile m_file = {};
  u64 m_file_size = 0;
  std::vector<u8> m_zlib_buffer = {};
  void* m_inflate = nullptr;  // libretro-common's rinflate, made once
  std::string m_file_name = {};
  bool m_valid = false;
};

}  // namespace DiscIO
