// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NFSBlob.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

namespace DiscIO
{
bool NFSFileReader::ReadKey(const std::string& path, const std::string& directory, Key* key_out)
{
  const std::string_view directory_without_trailing_slash =
      std::string_view(directory).substr(0, directory.size() - 1);

  std::string parent, parent_name, parent_extension;
  SplitPath(directory_without_trailing_slash, &parent, &parent_name, &parent_extension);

  if (parent_name + parent_extension != "content")
  {
    ERROR_LOG(DISCIO, "hif_000000.nfs is not in a directory named 'content': %s", path.c_str());
    return false;
  }

  const std::string key_path = parent + "code/htk.bin";
  File::DirectIOFile key_file(key_path, File::AccessMode::Read);
  if (!key_file.Read(key_out->data(), key_out->size()))
  {
    ERROR_LOG(DISCIO, "Failed to read from %s", key_path.c_str());
    return false;
  }

  return true;
}

std::vector<NFSLBARange> NFSFileReader::GetLBARanges(const NFSHeader& header)
{
  const size_t stored_count = Common::swap32(header.lba_range_count);
  const size_t lba_range_count =
      stored_count < header.lba_ranges.size() ? stored_count : header.lba_ranges.size();

  std::vector<NFSLBARange> lba_ranges;
  lba_ranges.reserve(lba_range_count);

  for (size_t i = 0; i < lba_range_count; ++i)
  {
    const NFSLBARange& unswapped_lba_range = header.lba_ranges[i];
    lba_ranges.push_back(NFSLBARange{Common::swap32(unswapped_lba_range.start_block),
                                     Common::swap32(unswapped_lba_range.num_blocks)});
  }

  return lba_ranges;
}

std::vector<File::DirectIOFile> NFSFileReader::OpenFiles(const std::string& directory,
                                                         File::DirectIOFile first_file,
                                                         u64 expected_raw_size, u64* raw_size_out)
{
  const u64 file_count = Common::AlignUp(expected_raw_size, MAX_FILE_SIZE) / MAX_FILE_SIZE;

  std::vector<File::DirectIOFile> files;
  files.reserve(file_count);

  *raw_size_out = first_file.GetSize();
  files.emplace_back(std::move(first_file));

  for (u64 i = 1; i < file_count; ++i)
  {
    char child_name[32];
    snprintf(child_name, sizeof(child_name), "hif_%06" PRIu64 ".nfs", i);
    const std::string child_path = directory + child_name;
    File::DirectIOFile child(child_path, File::AccessMode::Read);
    if (!child.IsOpen())
    {
      ERROR_LOG(DISCIO, "Failed to open %s", child_path.c_str());
      return {};
    }

    *raw_size_out += child.GetSize();
    files.emplace_back(std::move(child));
  }

  if (*raw_size_out < expected_raw_size)
  {
    ERROR_LOG(DISCIO,
              "Expected sum of NFS file sizes for %s to be at least %" PRIu64
              " bytes, but it was %" PRIu64 " bytes",
              directory.c_str(), expected_raw_size, *raw_size_out);
    return {};
  }

  return files;
}

u64 NFSFileReader::CalculateExpectedRawSize(const NFSLBARange* lba_ranges, size_t count)
{
  u64 total_blocks = 0;
  for (size_t i = 0; i < count; i++)
    total_blocks += lba_ranges[i].num_blocks;

  return sizeof(NFSHeader) + total_blocks * BLOCK_SIZE;
}

u64 NFSFileReader::CalculateExpectedDataSize(const NFSLBARange* lba_ranges, size_t count)
{
  u32 greatest_block_index = 0;
  for (size_t i = 0; i < count; i++)
  {
    const u32 end = lba_ranges[i].start_block + lba_ranges[i].num_blocks;
    if (end > greatest_block_index)
      greatest_block_index = end;
  }

  return u64(greatest_block_index) * BLOCK_SIZE;
}

NFSFileReader* NFSFileReader::Create(File::DirectIOFile first_file, const std::string& path)
{
  std::string directory, filename, extension;
  SplitPath(path, &directory, &filename, &extension);
  if (filename + extension != "hif_000000.nfs")
    return nullptr;

  std::array<u8, 16> key;
  if (!ReadKey(path, directory, &key))
    return nullptr;

  NFSHeader header;
  if (!first_file.OffsetRead(0, reinterpret_cast<u8*>(&header), sizeof(header)) || header.magic != NFS_MAGIC)
    return nullptr;

  std::vector<NFSLBARange> lba_ranges = GetLBARanges(header);

  const u64 expected_raw_size = CalculateExpectedRawSize(lba_ranges.data(), lba_ranges.size());

  u64 raw_size;
  std::vector<File::DirectIOFile> files =
      OpenFiles(directory, std::move(first_file), expected_raw_size, &raw_size);

  if (files.empty())
    return nullptr;

  return new NFSFileReader(std::move(lba_ranges), std::move(files), key, raw_size);
}

NFSFileReader::NFSFileReader(std::vector<NFSLBARange> lba_ranges,
                             std::vector<File::DirectIOFile> files, Key key, u64 raw_size)
    : m_lba_ranges(std::move(lba_ranges)), m_files(std::move(files)), m_raw_size(raw_size),
      m_key(key)
{
  aes_init(&m_aes_context, key.data(), key.size());
  m_data_size = CalculateExpectedDataSize(m_lba_ranges.data(), m_lba_ranges.size());
}

BlobReader* NFSFileReader::CopyReader() const
{
  return new NFSFileReader(m_lba_ranges, m_files, m_key, m_raw_size);
}

u64 NFSFileReader::GetDataSize() const
{
  return m_data_size;
}

u64 NFSFileReader::GetRawSize() const
{
  return m_raw_size;
}

u64 NFSFileReader::ToPhysicalBlockIndex(u64 logical_block_index) const
{
  u64 physical_blocks_so_far = 0;

  for (const NFSLBARange& range : m_lba_ranges)
  {
    if (logical_block_index >= range.start_block &&
        logical_block_index < range.start_block + range.num_blocks)
    {
      return physical_blocks_so_far + (logical_block_index - range.start_block);
    }

    physical_blocks_so_far += range.num_blocks;
  }

  return std::numeric_limits<u64>::max();
}

bool NFSFileReader::ReadEncryptedBlock(u64 physical_block_index)
{
  constexpr u64 BLOCKS_PER_FILE = MAX_FILE_SIZE / BLOCK_SIZE;

  const u64 file_index = physical_block_index / BLOCKS_PER_FILE;
  const u64 block_in_file = physical_block_index % BLOCKS_PER_FILE;
  const u64 offset_in_file = sizeof(NFSHeader) + block_in_file * BLOCK_SIZE;

  if (block_in_file == BLOCKS_PER_FILE - 1)
  {
    // Special case. Because of the 0x200 byte header at the very beginning,
    // the last block of each file has its last 0x200 bytes stored in the next file.

    constexpr size_t PART_1_SIZE = BLOCK_SIZE - sizeof(NFSHeader);
    constexpr size_t PART_2_SIZE = sizeof(NFSHeader);

    File::DirectIOFile& file_1 = m_files[file_index];
    File::DirectIOFile& file_2 = m_files[file_index + 1];

    if (!file_1.OffsetRead(offset_in_file, m_current_block_encrypted.data(), PART_1_SIZE))
      return false;

    if (!file_2.OffsetRead(0, m_current_block_encrypted.data() + PART_1_SIZE, PART_2_SIZE))
      return false;
  }
  else
  {
    // Normal case. The read is offset by 0x200 bytes, but it's all within one file.

    File::DirectIOFile& file = m_files[file_index];

    if (!file.OffsetRead(offset_in_file, m_current_block_encrypted.data(), BLOCK_SIZE))
    {
      return false;
    }
  }

  return true;
}

void NFSFileReader::DecryptBlock(u64 logical_block_index)
{
  std::array<u8, 16> iv{};
  const u64 swapped_block_index = Common::swap64(logical_block_index);
  std::memcpy(iv.data() + iv.size() - sizeof(swapped_block_index), &swapped_block_index,
              sizeof(swapped_block_index));

  aes_cbc_decrypt(&m_aes_context, iv.data(), m_current_block_encrypted.data(),
                  m_current_block_decrypted.data(), BLOCK_SIZE);
}

bool NFSFileReader::ReadAndDecryptBlock(u64 logical_block_index)
{
  const u64 physical_block_index = ToPhysicalBlockIndex(logical_block_index);

  if (physical_block_index == std::numeric_limits<u64>::max())
  {
    // The block isn't physically present. Treat its contents as all zeroes.
    m_current_block_decrypted.fill(0);
  }
  else
  {
    if (!ReadEncryptedBlock(physical_block_index))
      return false;

    DecryptBlock(logical_block_index);
  }

  // Small hack: Set 0x61 of the header to 1 so that VolumeWii realizes that the disc is unencrypted
  if (logical_block_index == 0)
    m_current_block_decrypted[0x61] = 1;

  return true;
}

bool NFSFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  while (nbytes != 0)
  {
    const u64 logical_block_index = offset / BLOCK_SIZE;
    const u64 offset_in_block = offset % BLOCK_SIZE;

    if (logical_block_index != m_current_logical_block_index)
    {
      if (!ReadAndDecryptBlock(logical_block_index))
        return false;

      m_current_logical_block_index = logical_block_index;
    }

    const u64 block_left = BLOCK_SIZE - offset_in_block;
    const u64 bytes_to_copy = nbytes < block_left ? nbytes : block_left;
    std::memcpy(out_ptr, m_current_block_decrypted.data() + offset_in_block, bytes_to_copy);

    offset += bytes_to_copy;
    nbytes -= bytes_to_copy;
    out_ptr += bytes_to_copy;
  }

  return true;
}

}  // namespace DiscIO
