// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Lazy.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class Country;
class FileSystem;
enum class Language;
enum class Region;
enum class Platform;

class VolumeGC final : public VolumeDisc
{
public:
  // takes reader
  VolumeGC(BlobReader* reader);
  VolumeGC(const VolumeGC&) = delete;
  VolumeGC& operator=(const VolumeGC&) = delete;
  ~VolumeGC() override;
  bool Read(u64 offset, u64 length, u8* buffer,
            const Partition& partition = PARTITION_NONE) const override;
  const u8* View(u64 offset, u64 length, const Partition& partition) const override;
  void Prefetch(u64 offset, u64 length, const Partition& partition) const override;
  const FileSystem* GetFileSystem(const Partition& partition = PARTITION_NONE) const override;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const override;

  Platform GetVolumeType() const override;
  bool IsDatelDisc() const override;
  Region GetRegion() const override;
  BlobType GetBlobType() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override;
  u64 GetRawSize() const override;
  const BlobReader& GetBlobReader() const override;

  const BootID* GetTriforceBootID() const { return &m_triforce_header; }

private:
  Common::Lazy<std::unique_ptr<FileSystem>> m_file_system;

  BlobReader* m_reader;  // owned

  BootID m_triforce_header;

  bool m_is_triforce;
};

}  // namespace DiscIO
