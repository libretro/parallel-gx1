// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeWii.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lrc_hash.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/ParallelFor.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
VolumeWii::VolumeWii(BlobReader* reader)
    : VolumeDisc(VolumeKind::Wii), m_reader(reader), m_game_partition(PARTITION_NONE),
      m_last_decrypted_block(UINT64_MAX)
{
  ASSERT(m_reader);

  m_has_hashes = m_reader->ReadSwapped<u8>(0x60) == u8(0);
  m_has_encryption = m_reader->ReadSwapped<u8>(0x61) == u8(0);

  if (m_has_encryption && !m_has_hashes)
    ERROR_LOG(DISCIO, "Wii disc has encryption but no hashes! This probably won't work well");

  for (u32 partition_group = 0; partition_group < 4; ++partition_group)
  {
    const std::optional<u32> number_of_partitions =
        m_reader->ReadSwapped<u32>(0x40000 + (partition_group * 8));
    if (!number_of_partitions)
      continue;

    const std::optional<u64> partition_table_offset =
        ReadSwappedAndShifted(0x40000 + (partition_group * 8) + 4, PARTITION_NONE);
    if (!partition_table_offset)
      continue;

    for (u32 i = 0; i < number_of_partitions; i++)
    {
      const std::optional<u64> partition_offset =
          ReadSwappedAndShifted(*partition_table_offset + (i * 8), PARTITION_NONE);
      if (!partition_offset)
        continue;

      const Partition partition(*partition_offset);

      const std::optional<u32> partition_type =
          m_reader->ReadSwapped<u32>(*partition_table_offset + (i * 8) + 4);
      if (!partition_type)
        continue;

      // If this is the game partition, set m_game_partition
      if (m_game_partition == PARTITION_NONE && *partition_type == 0)
        m_game_partition = partition;

      auto get_ticket = [this, partition]() -> IOS::ES::TicketReader {
        // read straight into the reader
        IOS::ES::TicketReader ticket;
        u8* const bytes = ticket.ResizeBytes(sizeof(IOS::ES::Ticket));
        if (!bytes || !m_reader->Read(partition.offset, sizeof(IOS::ES::Ticket), bytes))
          return INVALID_TICKET;
        return ticket;
      };

      auto get_tmd = [this, partition]() -> IOS::ES::TMDReader {
        const std::optional<u32> tmd_size =
            m_reader->ReadSwapped<u32>(partition.offset + WII_PARTITION_TMD_SIZE_ADDRESS);
        const std::optional<u64> tmd_address = ReadSwappedAndShifted(
            partition.offset + WII_PARTITION_TMD_OFFSET_ADDRESS, PARTITION_NONE);
        if (!tmd_size || !tmd_address)
          return INVALID_TMD;
        if (!IOS::ES::IsValidTMDSize(*tmd_size))
        {
          // This check is normally done by ES in ES_DiVerify, but that would happen too late
          // (after allocating the buffer), so we do the check here.
          ERROR_LOG(DISCIO, "Invalid TMD size");
          return INVALID_TMD;
        }
        IOS::ES::TMDReader tmd;
        u8* const bytes = tmd.ResizeBytes(*tmd_size);
        if (!bytes || !m_reader->Read(partition.offset + *tmd_address, *tmd_size, bytes))
          return INVALID_TMD;
        return tmd;
      };

      auto get_cert_chain = [this, partition]() -> std::vector<u8> {
        const std::optional<u32> size =
            m_reader->ReadSwapped<u32>(partition.offset + WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS);
        const std::optional<u64> address = ReadSwappedAndShifted(
            partition.offset + WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS, PARTITION_NONE);
        if (!size || !address)
          return {};
        std::vector<u8> cert_chain(*size);
        if (!m_reader->Read(partition.offset + *address, *size, cert_chain.data()))
          return {};
        return cert_chain;
      };

      auto get_h3_table = [this, partition]() -> std::vector<u8> {
        if (!m_has_hashes)
          return {};
        const std::optional<u64> h3_table_offset = ReadSwappedAndShifted(
            partition.offset + WII_PARTITION_H3_OFFSET_ADDRESS, PARTITION_NONE);
        if (!h3_table_offset)
          return {};
        std::vector<u8> h3_table(WII_PARTITION_H3_SIZE);
        if (!m_reader->Read(partition.offset + *h3_table_offset, WII_PARTITION_H3_SIZE,
                            h3_table.data()))
          return {};
        return h3_table;
      };

      auto get_key = [this, partition]() -> PartitionKey {
        const IOS::ES::TicketReader& ticket = *m_partitions[partition].ticket;
        PartitionKey key{};
        key.valid = ticket.IsValid() && !aes_init(&key.ctx, ticket.GetTitleKey().data(), 16);
        return key;
      };

      auto get_file_system = [this, partition]() -> FileSystem {
        return FileSystem(this, partition);
      };

      auto get_data_offset = [this, partition]() -> u64 {
        return ReadSwappedAndShifted(partition.offset + 0x2b8, PARTITION_NONE).value_or(0);
      };

      m_partitions.emplace(partition,
                           PartitionDetails{Common::Lazy<PartitionKey>(get_key),
                                            Common::Lazy<IOS::ES::TicketReader>(get_ticket),
                                            Common::Lazy<IOS::ES::TMDReader>(get_tmd),
                                            Common::Lazy<std::vector<u8>>(get_cert_chain),
                                            Common::Lazy<std::vector<u8>>(get_h3_table),
                                            Common::Lazy<FileSystem>(get_file_system),
                                            Common::Lazy<u64>(get_data_offset), *partition_type});
    }
  }
}

VolumeWii::~VolumeWii()
{
  DestroyBlobReader(m_reader);
}

bool VolumeWii::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  if (partition == PARTITION_NONE)
    return m_reader->Read(offset, length, buffer);

  auto it = m_partitions.find(partition);
  if (it == m_partitions.end())
    return false;
  const PartitionDetails& partition_details = it->second;

  const u64 partition_data_offset = partition.offset + *partition_details.data_offset;
  if (m_has_hashes && m_has_encryption &&
      m_reader->SupportsReadWiiDecrypted(offset, length, partition_data_offset))
  {
    return m_reader->ReadWiiDecrypted(offset, length, buffer, partition_data_offset);
  }

  if (!m_has_hashes)
  {
    return m_reader->Read(partition_data_offset + offset, length, buffer);
  }

  const struct aes_ctx* aes_context = nullptr;
  if (m_has_encryption)
  {
    if (!partition_details.key->valid)
      return false;
    aes_context = &partition_details.key->ctx;
  }

  while (length > 0)
  {
    // Calculate offsets
    u64 block_offset_on_disc = partition_data_offset + offset / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE;
    u64 data_offset_in_block = offset % BLOCK_DATA_SIZE;
    const u64 block_left = BLOCK_DATA_SIZE - data_offset_in_block;
    u64 copy_size = length < block_left ? length : block_left;

    if (m_last_decrypted_block != block_offset_on_disc)
    {
      // A whole block's data goes straight to the caller; a part goes through the
      // decrypted-block cache. An encrypted block is decrypted where it lies in the image
      // when the reader can lend it, else read into the volume's own block buffer.
      const bool whole = copy_size == BLOCK_DATA_SIZE;
      u8* const out = whole ? buffer : m_last_decrypted_block_data;
      if (m_has_encryption)
      {
        const u8* block = m_reader->View(block_offset_on_disc, BLOCK_TOTAL_SIZE);
        if (!block)
        {
          if (!m_reader->Read(block_offset_on_disc, BLOCK_TOTAL_SIZE, m_read_block))
            return false;
          block = m_read_block;
        }
        DecryptBlockData(block, out, aes_context);
      }
      else if (!m_reader->Read(block_offset_on_disc + BLOCK_HEADER_SIZE, BLOCK_DATA_SIZE, out))
      {
        return false;
      }

      if (whole)
      {
        length -= copy_size;
        buffer += copy_size;
        offset += copy_size;
        continue;
      }
      m_last_decrypted_block = block_offset_on_disc;
    }

    // Copy the decrypted data
    memcpy(buffer, &m_last_decrypted_block_data[data_offset_in_block],
           static_cast<size_t>(copy_size));

    // Update offsets
    length -= copy_size;
    buffer += copy_size;
    offset += copy_size;
  }

  return true;
}

bool VolumeWii::HasWiiHashes() const
{
  return m_has_hashes;
}

std::vector<Partition> VolumeWii::GetPartitions() const
{
  std::vector<Partition> partitions;
  partitions.reserve(m_partitions.size());
  for (const auto& partition : m_partitions)
    partitions.push_back(partition.first);
  return partitions;
}

Partition VolumeWii::GetGamePartition() const
{
  return m_game_partition;
}

std::optional<u64> VolumeWii::GetTitleID(const Partition& partition) const
{
  const IOS::ES::TicketReader& ticket = GetTicket(partition);
  if (!ticket.IsValid())
    return {};
  return ticket.GetTitleId();
}

const IOS::ES::TicketReader& VolumeWii::GetTicket(const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  return it != m_partitions.end() ? *it->second.ticket : INVALID_TICKET;
}

const IOS::ES::TMDReader& VolumeWii::GetTMD(const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  return it != m_partitions.end() ? *it->second.tmd : INVALID_TMD;
}

const std::vector<u8>& VolumeWii::GetCertificateChain(const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  return it != m_partitions.end() ? *it->second.cert_chain : INVALID_CERT_CHAIN;
}

const FileSystem* VolumeWii::GetFileSystem(const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  if (it == m_partitions.end())
    return nullptr;
  // nullptr when it could not be read
  const FileSystem& file_system = *it->second.file_system;
  return file_system.IsValid() ? &file_system : nullptr;
}

u64 VolumeWii::OffsetInHashedPartitionToRawOffset(u64 offset, const Partition& partition,
                                                  u64 partition_data_offset)
{
  if (partition == PARTITION_NONE)
    return offset;

  return partition.offset + partition_data_offset + (offset / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE) +
         (offset % BLOCK_DATA_SIZE);
}

u64 VolumeWii::PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  if (it == m_partitions.end())
    return offset;
  const u64 data_offset = *it->second.data_offset;

  if (!m_has_hashes)
    return partition.offset + data_offset + offset;

  return OffsetInHashedPartitionToRawOffset(offset, partition, data_offset);
}

std::string VolumeWii::GetGameTDBID(const Partition& partition) const
{
  return GetGameID(partition);
}

Region VolumeWii::GetRegion() const
{
  return RegionCodeToRegion(m_reader->ReadSwapped<u32>(0x4E000));
}

Platform VolumeWii::GetVolumeType() const
{
  return Platform::WiiDisc;
}

bool VolumeWii::IsDatelDisc() const
{
  return m_game_partition == PARTITION_NONE;
}

BlobType VolumeWii::GetBlobType() const
{
  return m_reader->GetBlobType();
}

u64 VolumeWii::GetDataSize() const
{
  return m_reader->GetDataSize();
}

DataSizeType VolumeWii::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeWii::GetRawSize() const
{
  return m_reader->GetRawSize();
}

const BlobReader& VolumeWii::GetBlobReader() const
{
  return *m_reader;
}

bool VolumeWii::CheckBlockIntegrity(u64 block_index, const u8* encrypted_data,
                                    const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  if (it == m_partitions.end())
    return false;
  const PartitionDetails& partition_details = it->second;

  if (block_index / BLOCKS_PER_GROUP * 20 >= partition_details.h3_table->size())
  {
    return false;
  }

  HashBlock hashes;
  u8 cluster_data_buffer[BLOCK_DATA_SIZE];
  const u8* cluster_data;

  if (m_has_encryption)
  {
    if (!partition_details.key->valid)
      return false;
    const struct aes_ctx* const aes_context = &partition_details.key->ctx;

    DecryptBlockHashes(encrypted_data, &hashes, aes_context);
    DecryptBlockData(encrypted_data, cluster_data_buffer, aes_context);
    cluster_data = cluster_data_buffer;
  }
  else
  {
    std::memcpy(&hashes, encrypted_data, BLOCK_HEADER_SIZE);
    cluster_data = encrypted_data + BLOCK_HEADER_SIZE;
  }

  u8 h0[31][20];
  u8 digest[20];
  SHA1DigestMany(cluster_data, 0x400, 0x400, 31, &h0[0][0]);
  if (std::memcmp(h0, hashes.h0, sizeof(h0)))
    return false;

  SHA1Digest(&hashes.h0[0][0], sizeof(hashes.h0), digest);
  if (std::memcmp(digest, hashes.h1[block_index % 8], sizeof(digest)))
    return false;

  SHA1Digest(&hashes.h1[0][0], sizeof(hashes.h1), digest);
  if (std::memcmp(digest, hashes.h2[block_index / 8 % 8], sizeof(digest)))
    return false;

  SHA1Digest(&hashes.h2[0][0], sizeof(hashes.h2), digest);
  return !std::memcmp(digest, partition_details.h3_table->data() + block_index / 64 * 20,
                      sizeof(digest));
}

bool VolumeWii::CheckBlockIntegrity(u64 block_index, const Partition& partition) const
{
  auto it = m_partitions.find(partition);
  if (it == m_partitions.end())
    return false;
  const PartitionDetails& partition_details = it->second;
  const u64 cluster_offset =
      partition.offset + *partition_details.data_offset + block_index * BLOCK_TOTAL_SIZE;

  std::vector<u8> cluster(BLOCK_TOTAL_SIZE);
  if (!m_reader->Read(cluster_offset, cluster.size(), cluster.data()))
    return false;
  return CheckBlockIntegrity(block_index, cluster.data(), partition);
}

namespace
{
struct HashJob
{
  const std::array<u8, VolumeWii::BLOCK_DATA_SIZE>* in;
  VolumeWii::HashBlock* out;
};

// H0 hashes of one block, and that block's entry in the H1 table of its subgroup.
void HashBlockJob(void* ctx, size_t i)
{
  const HashJob* const job = static_cast<const HashJob*>(ctx);
  VolumeWii::HashBlock* const out = job->out;
  const size_t h1_base = Common::AlignDown(i, 8);

  SHA1DigestMany(job->in[i].data(), 0x400, 0x400, 31, &out[i].h0[0][0]);
  out[i].padding_0 = {};
  SHA1Digest(&out[i].h0[0][0], sizeof(out[i].h0), out[h1_base].h1[i - h1_base]);
}

struct EncryptJob
{
  const std::array<u8, VolumeWii::BLOCK_DATA_SIZE>* in;
  const VolumeWii::HashBlock* hashes;
  u8* out;
  const struct aes_ctx* aes;
};

void EncryptBlockJob(void* ctx, size_t i)
{
  const EncryptJob* const job = static_cast<const EncryptJob*>(ctx);
  u8* const out_ptr = job->out + i * VolumeWii::BLOCK_TOTAL_SIZE;

  u8 iv[AES_BLOCK_SIZE] = {};

  aes_cbc_encrypt(job->aes, iv, reinterpret_cast<const u8*>(&job->hashes[i]), out_ptr,
                  VolumeWii::BLOCK_HEADER_SIZE);
  std::memcpy(iv, out_ptr + 0x3D0, AES_BLOCK_SIZE);
  aes_cbc_encrypt(job->aes, iv, job->in[i].data(), out_ptr + VolumeWii::BLOCK_HEADER_SIZE,
                  VolumeWii::BLOCK_DATA_SIZE);
}
}  // namespace

bool VolumeWii::HashGroup(const std::array<u8, BLOCK_DATA_SIZE> in[BLOCKS_PER_GROUP],
                          HashBlock out[BLOCKS_PER_GROUP],
                          const std::function<bool(size_t block)>& read_function)
{
  // The reads go through one blob reader, so they stay on this thread; the 64 blocks of hashing
  // are spread over the shared worker pool.
  if (read_function)
  {
    for (size_t i = 0; i < BLOCKS_PER_GROUP; ++i)
    {
      if (!read_function(i))
        return false;
    }
  }

  HashJob job{in, out};
  parallel_for(BLOCKS_PER_GROUP, HashBlockJob, &job);

  for (size_t h1_base = 0; h1_base < BLOCKS_PER_GROUP; h1_base += 8)
  {
    // H1 padding and copies
    out[h1_base].padding_1 = {};
    for (size_t j = 1; j < 8; ++j)
      std::memcpy(out[h1_base + j].h1, out[h1_base].h1, sizeof(out[h1_base].h1));

    // H2 hash
    SHA1Digest(&out[h1_base].h1[0][0], sizeof(out[h1_base].h1), out[0].h2[h1_base / 8]);
  }

  // H2 padding and copies
  out[0].padding_2 = {};
  for (size_t j = 1; j < BLOCKS_PER_GROUP; ++j)
    std::memcpy(out[j].h2, out[0].h2, sizeof(out[0].h2));

  return true;
}

bool VolumeWii::EncryptGroup(
    u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
    const std::array<u8, AES_KEY_SIZE>& key, BlobReader* blob,
    std::array<u8, GROUP_TOTAL_SIZE>* out,
    const std::function<void(HashBlock hash_blocks[BLOCKS_PER_GROUP])>& hash_exception_callback)
{
  std::vector<std::array<u8, BLOCK_DATA_SIZE>> unencrypted_data(BLOCKS_PER_GROUP);
  std::vector<HashBlock> unencrypted_hashes(BLOCKS_PER_GROUP);

  const bool success =
      HashGroup(unencrypted_data.data(), unencrypted_hashes.data(), [&](size_t block) {
        if (offset + (block + 1) * BLOCK_DATA_SIZE <= partition_data_decrypted_size)
        {
          if (!blob->ReadWiiDecrypted(offset + block * BLOCK_DATA_SIZE, BLOCK_DATA_SIZE,
                                      unencrypted_data[block].data(), partition_data_offset))
          {
            return false;
          }
        }
        else
        {
          unencrypted_data[block].fill(0);
        }
        return true;
      });

  if (!success)
    return false;

  if (hash_exception_callback)
    hash_exception_callback(unencrypted_hashes.data());

  struct aes_ctx aes_context;
  if (aes_init(&aes_context, key.data(), AES_KEY_SIZE))
    return false;

  EncryptJob job{unencrypted_data.data(), unencrypted_hashes.data(), out->data(), &aes_context};
  parallel_for(BLOCKS_PER_GROUP, EncryptBlockJob, &job);

  return true;
}

void VolumeWii::DecryptBlockHashes(const u8* in, HashBlock* out, const struct aes_ctx* aes_context)
{
  u8 iv[AES_BLOCK_SIZE] = {};
  aes_cbc_decrypt(aes_context, iv, in, reinterpret_cast<u8*>(out), sizeof(HashBlock));
}

void VolumeWii::DecryptBlockData(const u8* in, u8* out, const struct aes_ctx* aes_context)
{
  // the IV is the encrypted header's last bytes, copied first: out may be in
  u8 iv[AES_BLOCK_SIZE];
  std::memcpy(iv, &in[0x3d0], AES_BLOCK_SIZE);
  aes_cbc_decrypt(aes_context, iv, &in[sizeof(HashBlock)], out, BLOCK_DATA_SIZE);
}

}  // namespace DiscIO
