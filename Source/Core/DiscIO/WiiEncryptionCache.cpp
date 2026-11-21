// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WiiEncryptionCache.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
WiiEncryptionCache::WiiEncryptionCache(BlobReader* blob) : m_blob(blob)
{
}

WiiEncryptionCache::~WiiEncryptionCache()
{
  std::free(m_cache);
  std::free(m_unencrypted_data);
  std::free(m_unencrypted_hashes);
}

const u8* WiiEncryptionCache::EncryptGroup(u64 offset, u64 partition_data_offset,
                                           u64 partition_data_decrypted_size, const Key& key)
{
  // Only allocate memory if this function actually ends up getting called
  if (!m_cache)
  {
    m_cache = static_cast<u8*>(std::malloc(VolumeWii::GROUP_TOTAL_SIZE));
    m_unencrypted_data = static_cast<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>*>(
        std::malloc(VolumeWii::BLOCKS_PER_GROUP * sizeof(*m_unencrypted_data)));
    m_unencrypted_hashes = static_cast<VolumeWii::HashBlock*>(
        std::malloc(VolumeWii::BLOCKS_PER_GROUP * sizeof(*m_unencrypted_hashes)));
    if (!m_cache || !m_unencrypted_data || !m_unencrypted_hashes)
    {
      std::free(m_cache);
      std::free(m_unencrypted_data);
      std::free(m_unencrypted_hashes);
      m_cache = nullptr;
      m_unencrypted_data = nullptr;
      m_unencrypted_hashes = nullptr;
      return nullptr;
    }
    m_cached_offset = std::numeric_limits<u64>::max();
  }

  ASSERT(offset % VolumeWii::GROUP_TOTAL_SIZE == 0);
  const u64 group_offset_in_partition =
      offset / VolumeWii::GROUP_TOTAL_SIZE * VolumeWii::GROUP_DATA_SIZE;
  const u64 group_offset_on_disc = partition_data_offset + offset;

  if (m_cached_offset != group_offset_on_disc)
  {
    if (!VolumeWii::EncryptGroup(group_offset_in_partition, partition_data_offset,
                                 partition_data_decrypted_size, key, m_blob, m_cache,
                                 m_unencrypted_data, m_unencrypted_hashes))
    {
      m_cached_offset = std::numeric_limits<u64>::max();  // Invalidate the cache
      return nullptr;
    }

    m_cached_offset = group_offset_on_disc;
  }

  return m_cache;
}

bool WiiEncryptionCache::EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
                                       u64 partition_data_decrypted_size, const Key& key)
{
  while (size > 0)
  {
    const u8* const group = EncryptGroup(Common::AlignDown(offset, VolumeWii::GROUP_TOTAL_SIZE),
                                         partition_data_offset, partition_data_decrypted_size, key);

    if (!group)
      return false;

    const u64 offset_in_group = offset % VolumeWii::GROUP_TOTAL_SIZE;
    const u64 group_left = VolumeWii::GROUP_TOTAL_SIZE - offset_in_group;
    const u64 bytes_to_read = group_left < size ? group_left : size;
    std::memcpy(out_ptr, group + offset_in_group, bytes_to_read);

    offset += bytes_to_read;
    size -= bytes_to_read;
    out_ptr += bytes_to_read;
  }

  return true;
}

}  // namespace DiscIO
