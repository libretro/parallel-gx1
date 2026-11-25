// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include <crypto/crypto.h>
#include "Core/IOS/ES/Formats.h"
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

class VolumeWii final : public VolumeDisc
{
public:
  static constexpr size_t AES_KEY_SIZE = 16;

  static constexpr u32 BLOCKS_PER_GROUP = 0x40;

  static constexpr u64 BLOCK_HEADER_SIZE = 0x0400;
  static constexpr u64 BLOCK_DATA_SIZE = 0x7C00;
  static constexpr u64 BLOCK_TOTAL_SIZE = BLOCK_HEADER_SIZE + BLOCK_DATA_SIZE;

  static constexpr u64 GROUP_HEADER_SIZE = BLOCK_HEADER_SIZE * BLOCKS_PER_GROUP;
  static constexpr u64 GROUP_DATA_SIZE = BLOCK_DATA_SIZE * BLOCKS_PER_GROUP;
  static constexpr u64 GROUP_TOTAL_SIZE = GROUP_HEADER_SIZE + GROUP_DATA_SIZE;

  struct HashBlock
  {
    u8 h0[31][20];
    u8 padding_0[20];
    u8 h1[8][20];
    u8 padding_1[32];
    u8 h2[8][20];
    u8 padding_2[32];
  };
  static_assert(sizeof(HashBlock) == BLOCK_HEADER_SIZE);

  // takes reader
  VolumeWii(BlobReader* reader);
  VolumeWii(const VolumeWii&) = delete;
  VolumeWii& operator=(const VolumeWii&) = delete;
  ~VolumeWii();
  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const;
  bool HasWiiHashes() const;
  size_t GetPartitionCount() const { return m_partition_count; }
  Partition GetPartition(size_t index) const;
  Partition GetGamePartition() const;
  bool GetTitleID(const Partition& partition, u64* title_id) const;
  const IOS::ES::TicketReader& GetTicket(const Partition& partition) const;
  const IOS::ES::TMDReader& GetTMD(const Partition& partition) const;
  const std::vector<u8>& GetCertificateChain(const Partition& partition) const;
  const FileSystem* GetFileSystem(const Partition& partition) const;
  static u64 OffsetInHashedPartitionToRawOffset(u64 offset, const Partition& partition,
                                                u64 partition_data_offset);
  u64 PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const;

  Platform GetVolumeType() const;
  bool IsDatelDisc() const;

  Region GetRegion() const;
  BlobType GetBlobType() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;
  u64 GetRawSize() const;
  const BlobReader& GetBlobReader() const;

  // Hashes the blocks of a group, whose data is all in in.
  static void HashGroup(const u8 in[BLOCKS_PER_GROUP][BLOCK_DATA_SIZE],
                        HashBlock out[BLOCKS_PER_GROUP]);

  // Reads a group's decrypted blocks into unencrypted_data and its hashes into unencrypted_hashes,
  // the caller's scratch, then writes the encrypted group, GROUP_TOTAL_SIZE bytes, to out.
  static bool EncryptGroup(u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
                           const u8* key, BlobReader* blob, u8* out,
                           u8 (*unencrypted_data)[BLOCK_DATA_SIZE], HashBlock* unencrypted_hashes);

  static void DecryptBlockHashes(const u8* in, HashBlock* out, const struct aes_ctx* aes_context);
  static void DecryptBlockData(const u8* in, u8* out, const struct aes_ctx* aes_context);

protected:
  u32 GetOffsetShift() const { return 2; }

private:
  // Volume reaches the functions it has of its own here
  friend class Volume;

  // The title key of a partition, decrypting; not valid when the ticket is not
  struct PartitionKey
  {
    struct aes_ctx ctx;
    bool valid;
  };

  // A partition's details; the key and data offset are read up front since every read needs
  // them, the rest on first use
  struct PartitionDetails
  {
    Partition partition;
    u64 data_offset;
    PartitionKey key;
    IOS::ES::TicketReader ticket;
    mutable IOS::ES::TMDReader tmd;
    mutable std::vector<u8> cert_chain;
    mutable FileSystem* file_system;
    mutable bool tmd_read;
    mutable bool cert_chain_read;
  };

  const PartitionDetails* FindPartition(const Partition& partition) const;
  void LoadPartition(PartitionDetails* details, const Partition& partition);
  const IOS::ES::TMDReader& GetPartitionTMD(const PartitionDetails& details) const;
  const std::vector<u8>& GetPartitionCertChain(const PartitionDetails& details) const;
  const FileSystem& GetPartitionFileSystem(const PartitionDetails& details) const;

  BlobReader* m_reader;  // owned
  // Sorted by offset
  PartitionDetails* m_partitions;
  u32 m_partition_count;
  Partition m_game_partition;
  bool m_has_hashes;
  bool m_has_encryption;

  mutable u64 m_last_decrypted_block;
  mutable u8 m_last_decrypted_block_data[BLOCK_DATA_SIZE]{};
  // An encrypted block read from a reader that cannot lend it.
  mutable u8 m_read_block[BLOCK_TOTAL_SIZE];
};

}  // namespace DiscIO
