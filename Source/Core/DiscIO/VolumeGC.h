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
  ~VolumeGC();
  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition = PARTITION_NONE) const;
  const u8* View(u64 offset, u64 length, const Partition& partition) const;
  void Prefetch(u64 offset, u64 length, const Partition& partition) const;
  const FileSystem* GetFileSystem(const Partition& partition = PARTITION_NONE) const;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const;

  Platform GetVolumeType() const;
  bool IsDatelDisc() const;
  Region GetRegion() const;
  BlobType GetBlobType() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;
  u64 GetRawSize() const;
  const BlobReader& GetBlobReader() const;

  const BootID* GetTriforceBootID() const { return &m_triforce_header; }

private:
  // Volume reaches the functions it has of its own here
  friend class Volume;

  Common::Lazy<std::unique_ptr<FileSystem>> m_file_system;

  BlobReader* m_reader;  // owned

  BootID m_triforce_header;

  bool m_is_triforce;
};

}  // namespace DiscIO
