// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
class BlobReader;

class WiiEncryptionCache
{
public:
  using Key = std::array<u8, VolumeWii::AES_KEY_SIZE>;

  // The blob pointer is kept around for the lifetime of this object.
  explicit WiiEncryptionCache(BlobReader* blob);
  ~WiiEncryptionCache();

  WiiEncryptionCache(const WiiEncryptionCache&) = delete;
  WiiEncryptionCache& operator=(const WiiEncryptionCache&) = delete;
  WiiEncryptionCache(WiiEncryptionCache&&) = delete;
  WiiEncryptionCache& operator=(WiiEncryptionCache&&) = delete;

  // Encrypts exactly one group, VolumeWii::GROUP_TOTAL_SIZE bytes.
  // If the returned pointer is nullptr, reading from the blob failed.
  // If the returned pointer is not nullptr, it is guaranteed to be valid until
  // the next call of this function or the destruction of this object.
  const u8* EncryptGroup(u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
                         const Key& key);

  // Encrypts a variable number of groups, as determined by the offset and size parameters.
  // Supports reading groups partially.
  bool EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
                     u64 partition_data_decrypted_size, const Key& key);

private:
  BlobReader* m_blob;
  // Made on the first encryption and kept: the last group encrypted, and the decrypted blocks and
  // hashes a group is built from.
  u8* m_cache = nullptr;
  std::array<u8, VolumeWii::BLOCK_DATA_SIZE>* m_unencrypted_data = nullptr;
  VolumeWii::HashBlock* m_unencrypted_hashes = nullptr;
  u64 m_cached_offset = 0;
};

}  // namespace DiscIO
