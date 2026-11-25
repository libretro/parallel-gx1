// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <crypto/crypto.h>

#include <limits>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

// This is the file format used for Wii games released on the Wii U eShop.

namespace DiscIO
{
static constexpr u32 NFS_MAGIC = 0x53474745;  // "EGGS" (byteswapped to little endian)

struct NFSLBARange
{
  u32 start_block;
  u32 num_blocks;
};

struct NFSHeader
{
  u32 magic;  // EGGS
  u32 version;
  u32 unknown_1;
  u32 unknown_2;
  u32 lba_range_count;
  NFSLBARange lba_ranges[61];
  u32 end_magic;  // SGGE
};
static_assert(sizeof(NFSHeader) == 0x200);

class NFSFileReader final : public BlobReaderOfKind<BlobReaderKind::NFS>
{
public:
  static NFSFileReader* Create(File::DirectIOFile first_file, const std::string& directory_path);

  BlobType GetBlobType() const { return BlobType::NFS; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::LowerBound; }

  u64 GetBlockSize() const { return BLOCK_SIZE; }
  bool HasFastRandomAccessInBlock() const { return false; }
  std::string GetCompressionMethod() const { return {}; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);

private:
  static constexpr u32 BLOCK_SIZE = 0x8000;
  static constexpr u32 MAX_FILE_SIZE = 0xFA00000;

  // key_out: 16 bytes
  static bool ReadKey(const std::string& path, const std::string& directory, u8* key_out);
  // The header's ranges, byteswapped, into m_lba_ranges
  void GetLBARanges(const NFSHeader& header);
  // The hif_*.nfs files into m_files; false if one is missing or they are too small
  bool OpenFiles(const std::string& directory, File::DirectIOFile first_file,
                 u64 expected_raw_size);
  static u64 CalculateExpectedRawSize(const NFSLBARange* lba_ranges, size_t count);
  static u64 CalculateExpectedDataSize(const NFSLBARange* lba_ranges, size_t count);

  NFSFileReader() = default;
  void SetKey(const u8* key);

  u64 ToPhysicalBlockIndex(u64 logical_block_index) const;
  bool ReadEncryptedBlock(u64 physical_block_index);
  void DecryptBlock(u64 logical_block_index);
  bool ReadAndDecryptBlock(u64 logical_block_index);

  u8 m_current_block_encrypted[BLOCK_SIZE];
  u8 m_current_block_decrypted[BLOCK_SIZE];
  u64 m_current_logical_block_index = std::numeric_limits<u64>::max();

  NFSLBARange m_lba_ranges[61];  // as many as a header holds
  size_t m_lba_range_count = 0;
  // An 8.5 GB disc needs 35 files of MAX_FILE_SIZE
  static constexpr size_t MAX_FILES = 64;
  File::DirectIOFile m_files[MAX_FILES];
  size_t m_file_count = 0;
  struct aes_ctx m_aes_context;
  u64 m_raw_size = 0;
  u64 m_data_size = 0;
  u8 m_key[16] = {};
};

}  // namespace DiscIO
