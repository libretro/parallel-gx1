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
static constexpr u32 WBFS_MAGIC = 0x53464257;  // "WBFS" (byteswapped to little endian)

class WbfsFileReader final : public BlobReaderOfKind<BlobReaderKind::Wbfs>
{
public:
  ~WbfsFileReader();

  static WbfsFileReader* Create(File::DirectIOFile file, const char* path);

  BlobType GetBlobType() const { return BlobType::WBFS; }
  BlobReader* CopyReader() const;

  u64 GetRawSize() const { return m_size; }
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::UpperBound; }

  u64 GetBlockSize() const { return m_wbfs_sector_size; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr);

private:
  // path names the .wbfs file for its parts
  WbfsFileReader(File::DirectIOFile file, const char* path);
  WbfsFileReader() : m_size(0), m_good(false) {}
  // reads the header and the cluster table once the parts are in the list
  void Init();

  void OpenAdditionalFiles(const char* path);
  bool AddFileToList(File::DirectIOFile file);
  bool ReadHeader();

  File::DirectIOFile& SeekToCluster(u64 offset, u64* available);
  bool IsGood() const { return m_good; }
  struct FileEntry
  {
    File::DirectIOFile file;
    u64 base_address = 0;
    u64 size = 0;
  };

  // The .wbfs file and its .wbf1 to .wbf9 parts
  static constexpr size_t MAX_FILES = 10;
  FileEntry m_files[MAX_FILES];
  size_t m_file_count = 0;

  u64 m_size;

  u64 m_hd_sector_size;
  u64 m_wbfs_sector_size;
  u64 m_wbfs_sector_count;
  u64 m_disc_info_size;

#pragma pack(1)
  struct WbfsHeader
  {
    u32 magic;
    u32 hd_sector_count;
    u8 hd_sector_shift;
    u8 wbfs_sector_shift;
    u8 padding[2];
    u8 disc_table[500];
  } m_header;
#pragma pack()

  u16* m_wlba_table = nullptr;  // m_blocks_per_disc entries
  u64 m_blocks_per_disc;

  bool m_good;
};

}  // namespace DiscIO
