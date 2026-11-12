// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeGC.h"

#include <cinttypes>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
VolumeGC::VolumeGC(std::unique_ptr<BlobReader> reader)
    : m_reader(std::move(reader)), m_is_triforce(false)
{
  ASSERT(m_reader);

  m_file_system = [this]() -> std::unique_ptr<FileSystem> {
    auto file_system = std::make_unique<FileSystemGCWii>(this, PARTITION_NONE);
    return file_system->IsValid() ? std::move(file_system) : nullptr;
  };

  constexpr u32 BTID_MAGIC = 0x44495442;
  auto tmp_fs = GetFileSystem(PARTITION_NONE);
  if (tmp_fs)
  {
    std::unique_ptr<FileInfo> file_info = tmp_fs->FindFileInfo("boot.id");
    if (!file_info)
      return;
    const u64 file_size = ReadFile(*this, PARTITION_NONE, file_info.get(),
                                   reinterpret_cast<u8*>(&m_triforce_header), sizeof(BootID));
    if (file_size >= 4 && m_triforce_header.magic == BTID_MAGIC)
    {
      m_is_triforce = true;
    }
  }
}

VolumeGC::~VolumeGC() = default;

bool VolumeGC::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  if (partition != PARTITION_NONE)
    return false;

  return m_reader->Read(offset, length, buffer);
}

const u8* VolumeGC::View(u64 offset, u64 length, const Partition& partition) const
{
  return partition == PARTITION_NONE ? m_reader->View(offset, length) : nullptr;
}

void VolumeGC::Prefetch(u64 offset, u64 length, const Partition& partition) const
{
  if (partition == PARTITION_NONE)
    m_reader->Prefetch(offset, length);
}

const FileSystem* VolumeGC::GetFileSystem(const Partition& partition) const
{
  return m_file_system->get();
}

std::string VolumeGC::GetGameTDBID(const Partition& partition) const
{
  // Datel discs for the GameCube can have one of two different game IDs:
  //
  // 1: GNHE5d. (Yes, with a lowercase d.) This game ID is used not only for
  // all kinds of Datel discs, but also for the licensed release NHL Hitz 2002.
  //
  // 2: DTLX01. This game ID is used for a few late Datel releases. Both Action Replay
  // and FreeLoader are known to have been released under this game ID.
  //
  // Since no game ID used for Datel discs uniquely represents one product,
  // never use the game ID of a Datel disc for looking up the title or cover art.
  if (IsDatelDisc())
    return "";

  // Normal case. Just return the usual game ID.
  return GetGameID(partition);
}

Region VolumeGC::GetRegion() const
{
  if (m_is_triforce)
  {
    switch (m_triforce_header.region_flags & 0x000000FF)
    {
    default:
    case 0x02:  // JAPAN
    case 0x08:  // ASIA
      return Region::NTSC_J;
    case 0x0E:  // USA
      return Region::NTSC_U;
    case 0x0C:  // EXPORT
      return Region::PAL;
    }
  }

  return RegionCodeToRegion(m_reader->ReadSwapped<u32>(0x458));
}

BlobType VolumeGC::GetBlobType() const
{
  return m_reader->GetBlobType();
}

u64 VolumeGC::GetDataSize() const
{
  return m_reader->GetDataSize();
}

DataSizeType VolumeGC::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeGC::GetRawSize() const
{
  return m_reader->GetRawSize();
}

const BlobReader& VolumeGC::GetBlobReader() const
{
  return *m_reader;
}

Platform VolumeGC::GetVolumeType() const
{
  if (m_is_triforce)
    return Platform::Triforce;
  else
    return Platform::GameCubeDisc;
}

bool VolumeGC::IsDatelDisc() const
{
  return GetGameID() == "DTLX01" || !GetBootDOLOffset(*this, PARTITION_NONE).has_value();
}

}  // namespace DiscIO
