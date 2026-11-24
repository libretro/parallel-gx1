// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/DiscUtils.h"

#include <string>
#include <vector>


#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
bool GetBootDOLOffset(const Volume& volume, const Partition& partition, u64* dol_offset)
{
  const Platform volume_type = volume.GetVolumeType();
  if (!IsDisc(volume_type))
    return false;

  // Datel AR disc has 0x00000000 as the offset (invalid) and doesn't use it in the AppLoader.
  return volume.ReadSwappedAndShifted(0x420, partition, dol_offset) && *dol_offset != 0;
}

bool GetBootDOLSize(const Volume& volume, const Partition& partition, u64 dol_offset,
                    u32* dol_size_out)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;

  u32 dol_size = 0;

  // Iterate through the 7 code segments
  for (size_t i = 0; i < 7; i++)
  {
    u32 offset, size;
    if (!volume.ReadSwapped<u32>(dol_offset + 0x00 + i * 4, partition, &offset) ||
        !volume.ReadSwapped<u32>(dol_offset + 0x90 + i * 4, partition, &size))
      return false;
    if (offset + size > dol_size)
      dol_size = offset + size;
  }

  // Iterate through the 11 data segments
  for (size_t i = 0; i < 11; i++)
  {
    u32 offset, size;
    if (!volume.ReadSwapped<u32>(dol_offset + 0x1c + i * 4, partition, &offset) ||
        !volume.ReadSwapped<u32>(dol_offset + 0xac + i * 4, partition, &size))
      return false;
    if (offset + size > dol_size)
      dol_size = offset + size;
  }

  *dol_size_out = dol_size;
  return true;
}

bool GetFSTOffset(const Volume& volume, const Partition& partition, u64* fst_offset)
{
  const Platform volume_type = volume.GetVolumeType();
  if (!IsDisc(volume_type))
    return false;

  return volume.ReadSwappedAndShifted(0x424, partition, fst_offset);
}

bool GetFSTSize(const Volume& volume, const Partition& partition, u64* fst_size)
{
  const Platform volume_type = volume.GetVolumeType();
  if (!IsDisc(volume_type))
    return false;

  return volume.ReadSwappedAndShifted(0x428, partition, fst_size);
}

static u64 GetBiggestReferencedOffset(const Volume& volume, const Partition* partitions,
                                      size_t partition_count);

u64 GetBiggestReferencedOffset(const Volume& volume)
{
  std::vector<Partition> partitions = volume.GetPartitions();

  // If a partition doesn't seem to contain any valid data, skip it.
  // This can happen when certain programs that create WBFS files scrub the entirety of
  // the Masterpiece partitions in Super Smash Bros. Brawl without removing them from
  // the partition table. https://bugs.dolphin-emu.org/issues/8733
  size_t kept = 0;
  for (size_t i = 0; i < partitions.size(); i++)
  {
    u32 magic;
    if (volume.ReadSwapped<u32>(0x18, partitions[i], &magic) && magic == WII_DISC_MAGIC)
      partitions[kept++] = partitions[i];
  }
  partitions.resize(kept);

  if (partitions.empty())
    partitions.push_back(PARTITION_NONE);

  return GetBiggestReferencedOffset(volume, partitions.data(), partitions.size());
}

static u64 GetBiggestReferencedOffset(const Volume& volume, const FileInfo& file_info)
{
  if (file_info.IsDirectory())
  {
    u64 biggest_offset = 0;
    for (const FileInfo& f : file_info)
    {
      const u64 offset = GetBiggestReferencedOffset(volume, f);
      if (offset > biggest_offset)
        biggest_offset = offset;
    }
    return biggest_offset;
  }
  else
  {
    return file_info.GetOffset() + file_info.GetSize();
  }
}

static u64 GetBiggestReferencedOffset(const Volume& volume, const Partition* partitions,
                                      size_t partition_count)
{
  const u64 disc_header_size = volume.GetVolumeType() == Platform::GameCubeDisc ? 0x460 : 0x50000;
  u64 biggest_offset = disc_header_size;
  for (size_t i = 0; i < partition_count; i++)
  {
    const Partition& partition = partitions[i];
    if (partition != PARTITION_NONE)
    {
      const u64 offset = volume.PartitionOffsetToRawOffset(0x440, partition);
      if (offset > biggest_offset)
        biggest_offset = offset;
    }

    u64 dol_offset;
    if (GetBootDOLOffset(volume, partition, &dol_offset))
    {
      u32 dol_size;
      if (GetBootDOLSize(volume, partition, dol_offset, &dol_size))
      {
        const u64 offset = volume.PartitionOffsetToRawOffset(dol_offset + dol_size, partition);
        if (offset > biggest_offset)
        biggest_offset = offset;
      }
    }

    u64 fst_offset, fst_size;
    if (GetFSTOffset(volume, partition, &fst_offset) && GetFSTSize(volume, partition, &fst_size))
    {
      const u64 offset = volume.PartitionOffsetToRawOffset(fst_offset + fst_size, partition);
      if (offset > biggest_offset)
        biggest_offset = offset;
    }

    const FileSystem* fs = volume.GetFileSystem(partition);
    if (fs)
    {
      const u64 offset_in_partition = GetBiggestReferencedOffset(volume, fs->GetRoot());
      const u64 offset = volume.PartitionOffsetToRawOffset(offset_in_partition, partition);
      if (offset > biggest_offset)
        biggest_offset = offset;
    }
  }
  return biggest_offset;
}

}  // namespace DiscIO
