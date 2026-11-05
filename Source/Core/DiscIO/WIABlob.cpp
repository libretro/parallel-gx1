// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WIABlob.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <zstd.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/Swap.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/LaggedFibonacciGenerator.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WIACompression.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
template <bool RVZ>
WIARVZFileReader<RVZ>::WIARVZFileReader(File::DirectIOFile file, const std::string& path)
    : m_file(std::move(file)), m_path(path), m_encryption_cache(this)
{
  m_valid = Initialize(path);
}

template <bool RVZ>
WIARVZFileReader<RVZ>::~WIARVZFileReader() = default;

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Initialize(const std::string& path)
{
  if (!m_file.Seek(0, File::SeekOrigin::Begin) ||
      !m_file.Read(Common::AsWritableU8Span(m_header_1)))
  {
    return false;
  }

  if ((!RVZ && m_header_1.magic != WIA_MAGIC) || (RVZ && m_header_1.magic != RVZ_MAGIC))
    return false;

  const u32 version = RVZ ? RVZ_VERSION : WIA_VERSION;
  const u32 version_read_compatible =
      RVZ ? RVZ_VERSION_READ_COMPATIBLE : WIA_VERSION_READ_COMPATIBLE;

  const u32 file_version = Common::swap32(m_header_1.version);
  const u32 file_version_compatible = Common::swap32(m_header_1.version_compatible);

  if (version < file_version_compatible || version_read_compatible > file_version)
  {
    ERROR_LOG_FMT(DISCIO, "Unsupported version {} in {}", VersionToString(file_version), path);
    return false;
  }

  const auto header_1_actual_hash = Common::SHA1::CalculateDigest(
      reinterpret_cast<const u8*>(&m_header_1), sizeof(m_header_1) - Common::SHA1::DIGEST_LEN);
  if (m_header_1.header_1_hash != header_1_actual_hash)
    return false;

  if (Common::swap64(m_header_1.wia_file_size) != m_file.GetSize())
  {
    ERROR_LOG_FMT(DISCIO, "File size is incorrect for {}", path);
    return false;
  }

  const u32 header_2_size = Common::swap32(m_header_1.header_2_size);
  const u32 header_2_min_size = sizeof(WIAHeader2) - sizeof(WIAHeader2::compressor_data);
  if (header_2_size < header_2_min_size)
    return false;

  std::vector<u8> header_2(header_2_size);
  if (!m_file.Read(header_2))
    return false;

  const auto header_2_actual_hash = Common::SHA1::CalculateDigest(header_2);
  if (m_header_1.header_2_hash != header_2_actual_hash)
    return false;

  std::memcpy(&m_header_2, header_2.data(), std::min(header_2.size(), sizeof(WIAHeader2)));

  if (m_header_2.compressor_data_size > sizeof(WIAHeader2::compressor_data) ||
      header_2_size < header_2_min_size + m_header_2.compressor_data_size)
  {
    return false;
  }

  const u32 chunk_size = Common::swap32(m_header_2.chunk_size);
  const auto is_power_of_two = [](u32 x) { return (x & (x - 1)) == 0; };
  if ((!RVZ || chunk_size < VolumeWii::BLOCK_TOTAL_SIZE || !is_power_of_two(chunk_size)) &&
      chunk_size % VolumeWii::GROUP_TOTAL_SIZE != 0)
  {
    return false;
  }

  const u32 compression_type = Common::swap32(m_header_2.compression_type);
  m_compression_type = static_cast<WIARVZCompressionType>(compression_type);
  if (m_compression_type > (RVZ ? WIARVZCompressionType::Zstd : WIARVZCompressionType::LZMA2) ||
      (RVZ && m_compression_type == WIARVZCompressionType::Purge))
  {
    ERROR_LOG_FMT(DISCIO, "Unsupported compression type {} in {}", compression_type, path);
    return false;
  }

  const size_t number_of_partition_entries = Common::swap32(m_header_2.number_of_partition_entries);
  const size_t partition_entry_size = Common::swap32(m_header_2.partition_entry_size);
  std::vector<u8> partition_entries(partition_entry_size * number_of_partition_entries);
  if (!m_file.OffsetRead(Common::swap64(m_header_2.partition_entries_offset), partition_entries))
  {
    return false;
  }

  const auto partition_entries_actual_hash = Common::SHA1::CalculateDigest(partition_entries);
  if (m_header_2.partition_entries_hash != partition_entries_actual_hash)
    return false;

  const size_t copy_length = std::min(partition_entry_size, sizeof(PartitionEntry));
  const size_t memset_length = sizeof(PartitionEntry) - copy_length;
  u8* ptr = partition_entries.data();
  m_partition_entries.resize(number_of_partition_entries);
  for (size_t i = 0; i < number_of_partition_entries; ++i, ptr += partition_entry_size)
  {
    std::memcpy(&m_partition_entries[i], ptr, copy_length);
    std::memset(reinterpret_cast<u8*>(&m_partition_entries[i]) + copy_length, 0, memset_length);
  }

  for (size_t i = 0; i < m_partition_entries.size(); ++i)
  {
    const std::array<PartitionDataEntry, 2>& entries = m_partition_entries[i].data_entries;

    size_t non_empty_entries = 0;
    for (size_t j = 0; j < entries.size(); ++j)
    {
      const u32 number_of_sectors = Common::swap32(entries[j].number_of_sectors);
      if (number_of_sectors != 0)
      {
        ++non_empty_entries;

        const u32 last_sector = Common::swap32(entries[j].first_sector) + number_of_sectors;
        m_data_entries.emplace(last_sector * VolumeWii::BLOCK_TOTAL_SIZE, DataEntry(i, j));
      }
    }

    if (non_empty_entries > 1)
    {
      if (Common::swap32(entries[0].first_sector) > Common::swap32(entries[1].first_sector))
        return false;
    }
  }

  const u32 number_of_raw_data_entries = Common::swap32(m_header_2.number_of_raw_data_entries);
  m_raw_data_entries.resize(number_of_raw_data_entries);
  Chunk& raw_data_entries =
      ReadCompressedData(Common::swap64(m_header_2.raw_data_entries_offset),
                         Common::swap32(m_header_2.raw_data_entries_size),
                         number_of_raw_data_entries * sizeof(RawDataEntry), m_compression_type);
  if (!raw_data_entries.ReadAll(&m_raw_data_entries))
    return false;

  for (size_t i = 0; i < m_raw_data_entries.size(); ++i)
  {
    const RawDataEntry& entry = m_raw_data_entries[i];
    const u64 data_size = Common::swap64(entry.data_size);
    if (data_size != 0)
      m_data_entries.emplace(Common::swap64(entry.data_offset) + data_size, DataEntry(i));
  }

  const u32 number_of_group_entries = Common::swap32(m_header_2.number_of_group_entries);
  m_group_entries.resize(number_of_group_entries);
  Chunk& group_entries =
      ReadCompressedData(Common::swap64(m_header_2.group_entries_offset),
                         Common::swap32(m_header_2.group_entries_size),
                         number_of_group_entries * sizeof(GroupEntry), m_compression_type);
  if (!group_entries.ReadAll(&m_group_entries))
    return false;

  if (HasDataOverlap())
    return false;

  return true;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::HasDataOverlap() const
{
  for (size_t i = 0; i < m_partition_entries.size(); ++i)
  {
    const std::array<PartitionDataEntry, 2>& entries = m_partition_entries[i].data_entries;
    for (size_t j = 0; j < entries.size(); ++j)
    {
      if (Common::swap32(entries[j].number_of_sectors) == 0)
        continue;

      const u64 data_offset = Common::swap32(entries[j].first_sector) * VolumeWii::BLOCK_TOTAL_SIZE;
      const auto it = m_data_entries.upper_bound(data_offset);
      if (it == m_data_entries.end())
        return true;  // Not an overlap, but an error nonetheless
      if (!it->second.is_partition || it->second.index != i || it->second.partition_data_index != j)
        return true;  // Overlap
    }
  }

  for (size_t i = 0; i < m_raw_data_entries.size(); ++i)
  {
    if (Common::swap64(m_raw_data_entries[i].data_size) == 0)
      continue;

    const u64 data_offset = Common::swap64(m_raw_data_entries[i].data_offset);
    const auto it = m_data_entries.upper_bound(data_offset);
    if (it == m_data_entries.end())
      return true;  // Not an overlap, but an error nonetheless
    if (it->second.is_partition || it->second.index != i)
      return true;  // Overlap
  }

  return false;
}

template <bool RVZ>
std::unique_ptr<WIARVZFileReader<RVZ>> WIARVZFileReader<RVZ>::Create(File::DirectIOFile file,
                                                                     const std::string& path)
{
  std::unique_ptr<WIARVZFileReader> blob(new WIARVZFileReader(std::move(file), path));
  return blob->m_valid ? std::move(blob) : nullptr;
}

template <bool RVZ>
BlobType WIARVZFileReader<RVZ>::GetBlobType() const
{
  return RVZ ? BlobType::RVZ : BlobType::WIA;
}

template <bool RVZ>
std::unique_ptr<BlobReader> WIARVZFileReader<RVZ>::CopyReader() const
{
  return Create(m_file, m_path);
}

template <bool RVZ>
std::string WIARVZFileReader<RVZ>::GetCompressionMethod() const
{
  switch (m_compression_type)
  {
  case WIARVZCompressionType::Purge:
    return "Purge";
  case WIARVZCompressionType::Bzip2:
    return "bzip2";
  case WIARVZCompressionType::LZMA:
    return "LZMA";
  case WIARVZCompressionType::LZMA2:
    return "LZMA2";
  case WIARVZCompressionType::Zstd:
    return "Zstandard";
  default:
    return {};
  }
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Read(u64 offset, u64 size, u8* out_ptr)
{
  if (offset + size > Common::swap64(m_header_1.iso_file_size))
    return false;

  if (offset < sizeof(WIAHeader2::disc_header))
  {
    const u64 bytes_to_read = std::min(sizeof(WIAHeader2::disc_header) - offset, size);
    std::memcpy(out_ptr, m_header_2.disc_header.data() + offset, bytes_to_read);
    offset += bytes_to_read;
    size -= bytes_to_read;
    out_ptr += bytes_to_read;
  }

  const u32 chunk_size = Common::swap32(m_header_2.chunk_size);
  while (size > 0)
  {
    const auto it = m_data_entries.upper_bound(offset);
    if (it == m_data_entries.end())
      return false;

    const DataEntry& data = it->second;
    if (data.is_partition)
    {
      const PartitionEntry& partition = m_partition_entries[it->second.index];

      const u32 partition_first_sector = Common::swap32(partition.data_entries[0].first_sector);
      const u64 partition_data_offset = partition_first_sector * VolumeWii::BLOCK_TOTAL_SIZE;

      const u32 second_number_of_sectors =
          Common::swap32(partition.data_entries[1].number_of_sectors);
      const u32 partition_total_sectors =
          second_number_of_sectors ? Common::swap32(partition.data_entries[1].first_sector) -
                                         partition_first_sector + second_number_of_sectors :
                                     Common::swap32(partition.data_entries[0].number_of_sectors);

      for (const PartitionDataEntry& partition_data : partition.data_entries)
      {
        if (size == 0)
          return true;

        const u32 first_sector = Common::swap32(partition_data.first_sector);
        const u32 number_of_sectors = Common::swap32(partition_data.number_of_sectors);

        const u64 data_offset = first_sector * VolumeWii::BLOCK_TOTAL_SIZE;
        const u64 data_size = number_of_sectors * VolumeWii::BLOCK_TOTAL_SIZE;

        if (data_size == 0)
          continue;

        if (data_offset + data_size <= offset)
          continue;

        if (offset < data_offset)
          return false;

        const u64 bytes_to_read = std::min(data_size - (offset - data_offset), size);

        m_exception_list.clear();
        m_write_to_exception_list = true;
        m_exception_list_last_group_index = std::numeric_limits<u64>::max();
        Common::ScopeGuard guard([this] { m_write_to_exception_list = false; });

        bool hash_exception_error = false;
        if (!m_encryption_cache.EncryptGroups(
                offset - partition_data_offset, bytes_to_read, out_ptr, partition_data_offset,
                partition_total_sectors * VolumeWii::BLOCK_DATA_SIZE, partition.partition_key,
                [this, &hash_exception_error](
                    VolumeWii::HashBlock hash_blocks[VolumeWii::BLOCKS_PER_GROUP], u64 offset_) {
                  // EncryptGroups calls ReadWiiDecrypted, which calls ReadFromGroups,
                  // which populates m_exception_list when m_write_to_exception_list == true
                  if (!ApplyHashExceptions(m_exception_list, hash_blocks))
                    hash_exception_error = true;
                }))
        {
          return false;
        }
        if (hash_exception_error)
          return false;

        offset += bytes_to_read;
        size -= bytes_to_read;
        out_ptr += bytes_to_read;
      }
    }
    else
    {
      const RawDataEntry& raw_data = m_raw_data_entries[data.index];
      if (!ReadFromGroups(&offset, &size, &out_ptr, chunk_size, VolumeWii::BLOCK_TOTAL_SIZE,
                          Common::swap64(raw_data.data_offset), Common::swap64(raw_data.data_size),
                          Common::swap32(raw_data.group_index),
                          Common::swap32(raw_data.number_of_groups), 0))
      {
        return false;
      }
    }
  }

  return true;
}

template <bool RVZ>
const typename WIARVZFileReader<RVZ>::PartitionEntry*
WIARVZFileReader<RVZ>::GetPartition(u64 partition_data_offset, u32* partition_first_sector) const
{
  const auto it = m_data_entries.upper_bound(partition_data_offset);
  if (it == m_data_entries.end() || !it->second.is_partition)
    return nullptr;

  const PartitionEntry* partition = &m_partition_entries[it->second.index];
  *partition_first_sector = Common::swap32(partition->data_entries[0].first_sector);
  if (partition_data_offset != *partition_first_sector * VolumeWii::BLOCK_TOTAL_SIZE)
    return nullptr;

  return partition;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::SupportsReadWiiDecrypted(u64 offset, u64 size,
                                                     u64 partition_data_offset) const
{
  u32 partition_first_sector;
  const PartitionEntry* partition = GetPartition(partition_data_offset, &partition_first_sector);
  if (!partition)
    return false;

  for (const PartitionDataEntry& data : partition->data_entries)
  {
    const u32 start_sector = Common::swap32(data.first_sector) - partition_first_sector;
    const u32 end_sector = start_sector + Common::swap32(data.number_of_sectors);

    if (offset + size <= end_sector * VolumeWii::BLOCK_DATA_SIZE)
      return true;
  }

  return false;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr,
                                             u64 partition_data_offset)
{
  u32 partition_first_sector;
  const PartitionEntry* partition = GetPartition(partition_data_offset, &partition_first_sector);
  if (!partition)
    return false;

  const u64 chunk_size = Common::swap32(m_header_2.chunk_size) * VolumeWii::BLOCK_DATA_SIZE /
                         VolumeWii::BLOCK_TOTAL_SIZE;

  for (const PartitionDataEntry& data : partition->data_entries)
  {
    if (size == 0)
      return true;

    const u64 data_offset =
        (Common::swap32(data.first_sector) - partition_first_sector) * VolumeWii::BLOCK_DATA_SIZE;
    const u64 data_size = Common::swap32(data.number_of_sectors) * VolumeWii::BLOCK_DATA_SIZE;

    if (!ReadFromGroups(
            &offset, &size, &out_ptr, chunk_size, VolumeWii::BLOCK_DATA_SIZE, data_offset,
            data_size, Common::swap32(data.group_index), Common::swap32(data.number_of_groups),
            std::max<u32>(1, static_cast<u32>(chunk_size / VolumeWii::GROUP_DATA_SIZE))))
    {
      return false;
    }
  }

  return size == 0;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ReadFromGroups(u64* offset, u64* size, u8** out_ptr, u64 chunk_size,
                                           u32 sector_size, u64 data_offset, u64 data_size,
                                           u32 group_index, u32 number_of_groups,
                                           u32 exception_lists)
{
  if (data_offset + data_size <= *offset)
    return true;

  if (*offset < data_offset)
    return false;

  const u64 skipped_data = data_offset % sector_size;
  data_offset -= skipped_data;
  data_size += skipped_data;

  const u64 start_group_index = (*offset - data_offset) / chunk_size;
  for (u64 i = start_group_index; i < number_of_groups && (*size) > 0; ++i)
  {
    const u64 total_group_index = group_index + i;
    if (total_group_index >= m_group_entries.size())
      return false;

    const GroupEntry group = m_group_entries[total_group_index];
    const u64 group_offset_in_data = i * chunk_size;
    const u64 offset_in_group = *offset - group_offset_in_data - data_offset;

    chunk_size = std::min(chunk_size, data_size - group_offset_in_data);

    const u64 bytes_to_read = std::min(chunk_size - offset_in_group, *size);
    u32 group_data_size = Common::swap32(group.data_size);

    WIARVZCompressionType compression_type = m_compression_type;
    u32 rvz_packed_size = 0;
    if constexpr (RVZ)
    {
      if ((group_data_size & 0x80000000) == 0)
        compression_type = WIARVZCompressionType::None;

      group_data_size &= 0x7FFFFFFF;

      rvz_packed_size = Common::swap32(group.rvz_packed_size);
    }

    if (group_data_size == 0)
    {
      std::memset(*out_ptr, 0, bytes_to_read);
    }
    else
    {
      const u64 group_offset_in_file = static_cast<u64>(Common::swap32(group.data_offset)) << 2;

      Chunk& chunk =
          ReadCompressedData(group_offset_in_file, group_data_size, chunk_size, compression_type,
                             exception_lists, rvz_packed_size, group_offset_in_data);

      if (!chunk.Read(offset_in_group, bytes_to_read, *out_ptr))
      {
        m_cached_chunk_offset = std::numeric_limits<u64>::max();  // Invalidate the cache
        return false;
      }

      if (m_write_to_exception_list && m_exception_list_last_group_index != total_group_index)
      {
        const u64 exception_list_index = offset_in_group / VolumeWii::GROUP_DATA_SIZE;
        const u16 additional_offset =
            static_cast<u16>(group_offset_in_data % VolumeWii::GROUP_DATA_SIZE /
                             VolumeWii::BLOCK_DATA_SIZE * VolumeWii::BLOCK_HEADER_SIZE);
        chunk.GetHashExceptions(&m_exception_list, exception_list_index, additional_offset);
        m_exception_list_last_group_index = total_group_index;
      }
    }

    *offset += bytes_to_read;
    *size -= bytes_to_read;
    *out_ptr += bytes_to_read;
  }

  return true;
}

template <bool RVZ>
typename WIARVZFileReader<RVZ>::Chunk&
WIARVZFileReader<RVZ>::ReadCompressedData(u64 offset_in_file, u64 compressed_size,
                                          u64 decompressed_size,
                                          WIARVZCompressionType compression_type,
                                          u32 exception_lists, u32 rvz_packed_size, u64 data_offset)
{
  if (offset_in_file == m_cached_chunk_offset)
    return m_cached_chunk;

  std::unique_ptr<Decompressor> decompressor;
  switch (compression_type)
  {
  case WIARVZCompressionType::None:
    decompressor = std::make_unique<NoneDecompressor>();
    break;
  case WIARVZCompressionType::Purge:
    decompressor = std::make_unique<PurgeDecompressor>(rvz_packed_size == 0 ? decompressed_size :
                                                                              rvz_packed_size);
    break;
  case WIARVZCompressionType::Bzip2:
    decompressor = std::make_unique<Bzip2Decompressor>();
    break;
  case WIARVZCompressionType::LZMA:
    decompressor = std::make_unique<LZMADecompressor>(false, m_header_2.compressor_data,
                                                      m_header_2.compressor_data_size);
    break;
  case WIARVZCompressionType::LZMA2:
    decompressor = std::make_unique<LZMADecompressor>(true, m_header_2.compressor_data,
                                                      m_header_2.compressor_data_size);
    break;
  case WIARVZCompressionType::Zstd:
    decompressor = std::make_unique<ZstdDecompressor>();
    break;
  }

  const bool compressed_exception_lists = compression_type > WIARVZCompressionType::Purge;

  m_cached_chunk =
      Chunk(&m_file, offset_in_file, compressed_size, decompressed_size, exception_lists,
            compressed_exception_lists, rvz_packed_size, data_offset, std::move(decompressor));
  m_cached_chunk_offset = offset_in_file;
  return m_cached_chunk;
}

template <bool RVZ>
std::string WIARVZFileReader<RVZ>::VersionToString(u32 version)
{
  const u8 a = version >> 24;
  const u8 b = (version >> 16) & 0xff;
  const u8 c = (version >> 8) & 0xff;
  const u8 d = version & 0xff;

  if (d == 0 || d == 0xff)
    return fmt::format("{}.{:02x}.{:02x}", a, b, c);
  else
    return fmt::format("{}.{:02x}.{:02x}.beta{}", a, b, c, d);
}

template <bool RVZ>
WIARVZFileReader<RVZ>::Chunk::Chunk() = default;

template <bool RVZ>
WIARVZFileReader<RVZ>::Chunk::Chunk(File::DirectIOFile* file, u64 offset_in_file,
                                    u64 compressed_size, u64 decompressed_size, u32 exception_lists,
                                    bool compressed_exception_lists, u32 rvz_packed_size,
                                    u64 data_offset, std::unique_ptr<Decompressor> decompressor)
    : m_decompressor(std::move(decompressor)), m_file(file), m_offset_in_file(offset_in_file),
      m_exception_lists(exception_lists), m_compressed_exception_lists(compressed_exception_lists),
      m_rvz_packed_size(rvz_packed_size), m_data_offset(data_offset)
{
  constexpr size_t MAX_SIZE_PER_EXCEPTION_LIST =
      Common::AlignUp(VolumeWii::BLOCK_HEADER_SIZE, Common::SHA1::DIGEST_LEN) /
          Common::SHA1::DIGEST_LEN * VolumeWii::BLOCKS_PER_GROUP * sizeof(HashExceptionEntry) +
      sizeof(u16);

  m_out_bytes_allocated_for_exceptions =
      m_compressed_exception_lists ? MAX_SIZE_PER_EXCEPTION_LIST * m_exception_lists : 0;

  m_in.data.resize(compressed_size);
  m_out.data.resize(decompressed_size + m_out_bytes_allocated_for_exceptions);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::Read(u64 offset, u64 size, u8* out_ptr)
{
  if (!m_decompressor || !m_file ||
      offset + size > m_out.data.size() - m_out_bytes_allocated_for_exceptions)
  {
    return false;
  }

  while (offset + size > GetOutBytesWrittenExcludingExceptions())
  {
    u64 bytes_to_read;
    if (offset + size == m_out.data.size())
    {
      // Read all the remaining data.
      bytes_to_read = m_in.data.size() - m_in.bytes_written;
    }
    else
    {
      // Pick a suitable amount of compressed data to read. We have to ensure that bytes_to_read
      // is larger than 0 and smaller than or equal to the number of bytes available to read,
      // but the rest is a bit arbitrary and could be changed.

      // The compressed data is probably not much bigger than the decompressed data.
      // Add a few bytes for possible compression overhead and for any hash exceptions.
      bytes_to_read = offset + size - GetOutBytesWrittenExcludingExceptions() + 0x100;

      // Align the access in an attempt to gain speed. But we don't actually know the
      // block size of the underlying storage device, so we just use the Wii block size.
      bytes_to_read =
          Common::AlignUp(bytes_to_read + m_offset_in_file, VolumeWii::BLOCK_TOTAL_SIZE) -
          m_offset_in_file;

      // Ensure we don't read too much.
      bytes_to_read = std::min<u64>(m_in.data.size() - m_in.bytes_written, bytes_to_read);
    }

    if (bytes_to_read == 0)
    {
      // Compressed size is larger than expected or decompressed size is smaller than expected
      return false;
    }

    if (!m_file->OffsetRead(m_offset_in_file, m_in.data.data() + m_in.bytes_written, bytes_to_read))
      return false;

    m_offset_in_file += bytes_to_read;
    m_in.bytes_written += bytes_to_read;

    if (m_exception_lists > 0 && !m_compressed_exception_lists)
    {
      if (!HandleExceptions(m_in.data.data(), m_in.data.size(), m_in.bytes_written,
                            &m_in_bytes_used_for_exceptions, true))
      {
        return false;
      }

      m_in_bytes_read = m_in_bytes_used_for_exceptions;
    }

    if (m_exception_lists == 0 || m_compressed_exception_lists)
    {
      if (!Decompress())
        return false;
    }

    if (m_exception_lists > 0 && m_compressed_exception_lists)
    {
      if (!HandleExceptions(m_out.data.data(), m_out_bytes_allocated_for_exceptions,
                            m_out.bytes_written, &m_out_bytes_used_for_exceptions, false))
      {
        return false;
      }

      if (m_rvz_packed_size != 0 && m_exception_lists == 0)
      {
        if (!Decompress())
          return false;
      }
    }

    if (m_exception_lists == 0)
    {
      const size_t expected_out_bytes = m_out.data.size() - m_out_bytes_allocated_for_exceptions +
                                        m_out_bytes_used_for_exceptions;

      if (m_out.bytes_written > expected_out_bytes)
        return false;  // Decompressed size is larger than expected

      // The reason why we need the m_in.bytes_written == m_in.data.size() check as part of
      // this conditional is because (for example) zstd can finish writing all data to m_out
      // before becoming done if we've given it all input data except the checksum at the end.
      if (m_out.bytes_written == expected_out_bytes && !m_decompressor->Done() &&
          m_in.bytes_written == m_in.data.size())
      {
        return false;  // Decompressed size is larger than expected
      }

      if (m_decompressor->Done() && m_in_bytes_read != m_in.data.size())
        return false;  // Compressed size is smaller than expected
    }
  }

  std::memcpy(out_ptr, m_out.data.data() + offset + m_out_bytes_used_for_exceptions, size);
  return true;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::Decompress()
{
  if (m_rvz_packed_size != 0 && m_exception_lists == 0)
  {
    const size_t bytes_to_move = m_out.bytes_written - m_out_bytes_used_for_exceptions;

    DecompressionBuffer in{std::vector<u8>(bytes_to_move), bytes_to_move};

    // Copying to a null pointer is undefined behaviour, so only copy when we
    // actually have data to copy.
    if (bytes_to_move > 0)
    {
      std::memcpy(in.data.data(), m_out.data.data() + m_out_bytes_used_for_exceptions,
                  bytes_to_move);
    }

    m_out.bytes_written = m_out_bytes_used_for_exceptions;

    m_decompressor = std::make_unique<RVZPackDecompressor>(std::move(m_decompressor), std::move(in),
                                                           m_data_offset, m_rvz_packed_size);

    m_rvz_packed_size = 0;
  }

  return m_decompressor->Decompress(m_in, &m_out, &m_in_bytes_read);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::HandleExceptions(const u8* data, size_t bytes_allocated,
                                                    size_t bytes_written, size_t* bytes_used,
                                                    bool align)
{
  while (m_exception_lists > 0)
  {
    if (sizeof(u16) + *bytes_used > bytes_allocated)
    {
      ERROR_LOG_FMT(DISCIO, "More hash exceptions than expected");
      return false;
    }
    if (sizeof(u16) + *bytes_used > bytes_written)
      return true;

    const u16 exceptions = Common::swap16(data + *bytes_used);

    size_t exception_list_size = exceptions * sizeof(HashExceptionEntry) + sizeof(u16);
    if (align && m_exception_lists == 1)
      exception_list_size = Common::AlignUp(*bytes_used + exception_list_size, 4) - *bytes_used;

    if (exception_list_size + *bytes_used > bytes_allocated)
    {
      ERROR_LOG_FMT(DISCIO, "More hash exceptions than expected");
      return false;
    }
    if (exception_list_size + *bytes_used > bytes_written)
      return true;

    *bytes_used += exception_list_size;
    --m_exception_lists;
  }

  return true;
}

template <bool RVZ>
void WIARVZFileReader<RVZ>::Chunk::GetHashExceptions(
    std::vector<HashExceptionEntry>* exception_list, u64 exception_list_index,
    u16 additional_offset) const
{
  ASSERT(m_exception_lists == 0);

  const u8* data_start = m_compressed_exception_lists ? m_out.data.data() : m_in.data.data();
  const u8* data = data_start;

  for (u64 i = exception_list_index; i > 0; --i)
    data += Common::swap16(data) * sizeof(HashExceptionEntry) + sizeof(u16);

  const u16 exceptions = Common::swap16(data);
  data += sizeof(u16);

  for (size_t i = 0; i < exceptions; ++i)
  {
    std::memcpy(&exception_list->emplace_back(), data, sizeof(HashExceptionEntry));
    data += sizeof(HashExceptionEntry);

    u16& offset = exception_list->back().offset;
    offset = Common::swap16(Common::swap16(offset) + additional_offset);
  }

  ASSERT(data <= data_start + (m_compressed_exception_lists ? m_out_bytes_used_for_exceptions :
                                                              m_in_bytes_used_for_exceptions));
}

template <bool RVZ>
size_t WIARVZFileReader<RVZ>::Chunk::GetOutBytesWrittenExcludingExceptions() const
{
  return m_exception_lists == 0 ? m_out.bytes_written - m_out_bytes_used_for_exceptions : 0;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ApplyHashExceptions(
    std::span<const HashExceptionEntry> exception_list,
    VolumeWii::HashBlock hash_blocks[VolumeWii::BLOCKS_PER_GROUP])
{
  for (const HashExceptionEntry& exception : exception_list)
  {
    const u16 offset = Common::swap16(exception.offset);

    const size_t block_index = offset / VolumeWii::BLOCK_HEADER_SIZE;
    if (block_index > VolumeWii::BLOCKS_PER_GROUP)
      return false;

    const size_t offset_in_block = offset % VolumeWii::BLOCK_HEADER_SIZE;
    if (offset_in_block + Common::SHA1::DIGEST_LEN > VolumeWii::BLOCK_HEADER_SIZE)
      return false;

    std::memcpy(reinterpret_cast<u8*>(&hash_blocks[block_index]) + offset_in_block, &exception.hash,
                Common::SHA1::DIGEST_LEN);
  }

  return true;
}

template class WIARVZFileReader<false>;
template class WIARVZFileReader<true>;

}  // namespace DiscIO
