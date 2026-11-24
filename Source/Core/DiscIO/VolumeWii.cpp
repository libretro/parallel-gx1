// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeWii.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
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
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
VolumeWii::VolumeWii(BlobReader* reader)
    : VolumeDisc(VolumeKind::Wii), m_reader(reader), m_partitions(nullptr), m_partition_count(0),
      m_game_partition(PARTITION_NONE), m_last_decrypted_block(UINT64_MAX)
{
  ASSERT(m_reader);

  u8 flag;
  m_has_hashes = m_reader->ReadSwapped<u8>(0x60, &flag) && flag == 0;
  m_has_encryption = m_reader->ReadSwapped<u8>(0x61, &flag) && flag == 0;

  if (m_has_encryption && !m_has_hashes)
    ERROR_LOG(DISCIO, "Wii disc has encryption but no hashes! This probably won't work well");

  // The partition offsets, sorted, without repeats
  u64* offsets = nullptr;
  u32 count = 0;
  u32 capacity = 0;

  for (u32 partition_group = 0; partition_group < 4; ++partition_group)
  {
    u32 number_of_partitions;
    if (!m_reader->ReadSwapped<u32>(0x40000 + (partition_group * 8), &number_of_partitions))
      continue;

    u64 partition_table_offset;
    if (!ReadSwappedAndShifted(0x40000 + (partition_group * 8) + 4, PARTITION_NONE,
                               &partition_table_offset))
      continue;

    for (u32 i = 0; i < number_of_partitions; i++)
    {
      u64 partition_offset;
      if (!ReadSwappedAndShifted(partition_table_offset + (i * 8), PARTITION_NONE,
                                 &partition_offset))
        continue;

      u32 partition_type;
      if (!m_reader->ReadSwapped<u32>(partition_table_offset + (i * 8) + 4, &partition_type))
        continue;

      // If this is the game partition, set m_game_partition
      if (m_game_partition == PARTITION_NONE && partition_type == 0)
        m_game_partition = Partition(partition_offset);

      u32 at = count;
      while (at > 0 && offsets[at - 1] > partition_offset)
        at--;
      if (at > 0 && offsets[at - 1] == partition_offset)
        continue;

      if (count == capacity)
      {
        const u32 new_capacity = capacity ? capacity * 2 : 8;
        u64* const grown = static_cast<u64*>(std::realloc(offsets, new_capacity * sizeof(u64)));
        if (!grown)
          continue;
        offsets = grown;
        capacity = new_capacity;
      }
      std::memmove(offsets + at + 1, offsets + at, (count - at) * sizeof(u64));
      offsets[at] = partition_offset;
      count++;
    }
  }

  if (count)
  {
    m_partitions = new PartitionDetails[count];
    m_partition_count = count;
    for (u32 i = 0; i < count; i++)
      LoadPartition(&m_partitions[i], Partition(offsets[i]));
  }
  std::free(offsets);
}

void VolumeWii::LoadPartition(PartitionDetails* details, const Partition& partition)
{
  details->partition = partition;
  if (!ReadSwappedAndShifted(partition.offset + 0x2b8, PARTITION_NONE, &details->data_offset))
    details->data_offset = 0;
  details->file_system = nullptr;
  details->tmd_read = false;
  details->cert_chain_read = false;

  // read straight into the reader
  u8* const bytes = details->ticket.ResizeBytes(sizeof(IOS::ES::Ticket));
  if (!bytes || !m_reader->Read(partition.offset, sizeof(IOS::ES::Ticket), bytes))
    details->ticket = INVALID_TICKET;

  // Only an encrypted disc's reads use the key
  std::memset(&details->key, 0, sizeof(details->key));
  details->key.valid = m_has_encryption && details->ticket.IsValid() &&
                       !aes_init(&details->key.ctx, details->ticket.GetTitleKey().data(), 16);
}

const VolumeWii::PartitionDetails* VolumeWii::FindPartition(const Partition& partition) const
{
  for (u32 i = 0; i < m_partition_count; i++)
  {
    if (m_partitions[i].partition == partition)
      return &m_partitions[i];
  }
  return nullptr;
}

const IOS::ES::TMDReader& VolumeWii::GetPartitionTMD(const PartitionDetails& details) const
{
  if (details.tmd_read)
    return details.tmd;
  details.tmd_read = true;

  const u64 offset = details.partition.offset;
  u32 tmd_size;
  u64 tmd_address;
  if (!m_reader->ReadSwapped<u32>(offset + WII_PARTITION_TMD_SIZE_ADDRESS, &tmd_size) ||
      !ReadSwappedAndShifted(offset + WII_PARTITION_TMD_OFFSET_ADDRESS, PARTITION_NONE,
                             &tmd_address))
    return details.tmd;
  if (!IOS::ES::IsValidTMDSize(tmd_size))
  {
    // This check is normally done by ES in ES_DiVerify, but that would happen too late
    // (after allocating the buffer), so we do the check here.
    ERROR_LOG(DISCIO, "Invalid TMD size");
    return details.tmd;
  }
  u8* const bytes = details.tmd.ResizeBytes(tmd_size);
  if (!bytes || !m_reader->Read(offset + tmd_address, tmd_size, bytes))
    details.tmd = INVALID_TMD;
  return details.tmd;
}

const std::vector<u8>& VolumeWii::GetPartitionCertChain(const PartitionDetails& details) const
{
  if (details.cert_chain_read)
    return details.cert_chain;
  details.cert_chain_read = true;

  const u64 offset = details.partition.offset;
  u32 size;
  u64 address;
  if (!m_reader->ReadSwapped<u32>(offset + WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS, &size) ||
      !ReadSwappedAndShifted(offset + WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS, PARTITION_NONE,
                             &address))
    return details.cert_chain;
  details.cert_chain.resize(size);
  if (!m_reader->Read(offset + address, size, details.cert_chain.data()))
    details.cert_chain.clear();
  return details.cert_chain;
}

const FileSystem& VolumeWii::GetPartitionFileSystem(const PartitionDetails& details) const
{
  if (!details.file_system)
    details.file_system = new FileSystem(this, details.partition);
  return *details.file_system;
}

VolumeWii::~VolumeWii()
{
  for (u32 i = 0; i < m_partition_count; i++)
    delete m_partitions[i].file_system;
  delete[] m_partitions;
  DestroyBlobReader(m_reader);
}

bool VolumeWii::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  if (partition == PARTITION_NONE)
    return m_reader->Read(offset, length, buffer);

  const PartitionDetails* const partition_details = FindPartition(partition);
  if (!partition_details)
    return false;

  const u64 partition_data_offset = partition.offset + partition_details->data_offset;
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
    if (!partition_details->key.valid)
      return false;
    aes_context = &partition_details->key.ctx;
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
  partitions.reserve(m_partition_count);
  for (u32 i = 0; i < m_partition_count; i++)
    partitions.push_back(m_partitions[i].partition);
  return partitions;
}

Partition VolumeWii::GetGamePartition() const
{
  return m_game_partition;
}

bool VolumeWii::GetTitleID(const Partition& partition, u64* title_id) const
{
  const IOS::ES::TicketReader& ticket = GetTicket(partition);
  if (!ticket.IsValid())
    return false;
  *title_id = ticket.GetTitleId();
  return true;
}

const IOS::ES::TicketReader& VolumeWii::GetTicket(const Partition& partition) const
{
  const PartitionDetails* const details = FindPartition(partition);
  return details ? details->ticket : INVALID_TICKET;
}

const IOS::ES::TMDReader& VolumeWii::GetTMD(const Partition& partition) const
{
  const PartitionDetails* const details = FindPartition(partition);
  return details ? GetPartitionTMD(*details) : INVALID_TMD;
}

const std::vector<u8>& VolumeWii::GetCertificateChain(const Partition& partition) const
{
  const PartitionDetails* const details = FindPartition(partition);
  return details ? GetPartitionCertChain(*details) : INVALID_CERT_CHAIN;
}

const FileSystem* VolumeWii::GetFileSystem(const Partition& partition) const
{
  const PartitionDetails* const details = FindPartition(partition);
  if (!details)
    return nullptr;
  // nullptr when it could not be read
  const FileSystem& file_system = GetPartitionFileSystem(*details);
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
  const PartitionDetails* const details = FindPartition(partition);
  if (!details)
    return offset;
  const u64 data_offset = details->data_offset;

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
  u32 region_code;
  if (!m_reader->ReadSwapped<u32>(0x4E000, &region_code))
    return Region::Unknown;
  return RegionCodeToRegion(region_code);
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

void VolumeWii::HashGroup(const std::array<u8, BLOCK_DATA_SIZE> in[BLOCKS_PER_GROUP],
                          HashBlock out[BLOCKS_PER_GROUP])
{
  // The 64 blocks of hashing are spread over the shared worker pool.
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
}

bool VolumeWii::EncryptGroup(u64 offset, u64 partition_data_offset,
                             u64 partition_data_decrypted_size,
                             const std::array<u8, AES_KEY_SIZE>& key, BlobReader* blob, u8* out,
                             std::array<u8, BLOCK_DATA_SIZE>* unencrypted_data,
                             HashBlock* unencrypted_hashes)
{
  // The reads go through one blob reader, so they stay on this thread.
  for (size_t block = 0; block < BLOCKS_PER_GROUP; ++block)
  {
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
  }

  HashGroup(unencrypted_data, unencrypted_hashes);

  struct aes_ctx aes_context;
  if (aes_init(&aes_context, key.data(), AES_KEY_SIZE))
    return false;

  EncryptJob job{unencrypted_data, unencrypted_hashes, out, &aes_context};
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
