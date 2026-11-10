// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

struct rvz;
struct rvz_request;

namespace DiscIO
{
constexpr u32 WIA_MAGIC = 0x01414957;  // "WIA\x1" (byteswapped to little endian)
constexpr u32 RVZ_MAGIC = 0x015A5652;  // "RVZ\x1" (byteswapped to little endian)

// WIA and RVZ images, read through libretro-common's formats/rvz
class WIARVZFileReader final : public BlobReader
{
public:
  ~WIARVZFileReader() override;

  static std::unique_ptr<WIARVZFileReader> Create(File::DirectIOFile file, const std::string& path);

  BlobType GetBlobType() const override { return m_rvz ? BlobType::RVZ : BlobType::WIA; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_raw_size; }
  u64 GetDataSize() const override { return m_data_size; }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return m_block_size; }
  bool HasFastRandomAccessInBlock() const override { return false; }
  std::string GetCompressionMethod() const override;
  std::optional<int> GetCompressionLevel() const override { return m_level; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override;

private:
  WIARVZFileReader(File::DirectIOFile file, const std::string& path, bool rvz);
  bool Fetch(const rvz_request* req);
  bool Open();
  bool RunRead(int armed, u64 size);

  File::DirectIOFile m_file;
  std::string m_path;
  struct rvz* m_reader = nullptr;
  bool m_rvz;
  int m_compression = 0;
  int m_level = 0;
  u64 m_raw_size = 0;
  u64 m_data_size = 0;
  u64 m_block_size = 0;
};

}  // namespace DiscIO
