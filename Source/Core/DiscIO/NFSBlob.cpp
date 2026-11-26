// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NFSBlob.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <retro_miscellaneous.h>
#include <string>
#include <utility>

#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

namespace DiscIO
{
// Where the name in the first length chars of path starts, after its last separator
static size_t NameStart(const char* path, size_t length)
{
  while (length > 0 && path[length - 1] != '/'
#ifdef _WIN32
         && path[length - 1] != ':'
#endif
  )
  {
    length--;
  }
  return length;
}

bool NFSFileReader::ReadKey(const char* path, size_t directory_length, u8* key_out)
{
  // the directory without its trailing slash, then its parent's
  const size_t content_length = directory_length ? directory_length - 1 : 0;
  const size_t parent_length = NameStart(path, content_length);
  static const char content[] = "content";

  if (!directory_length || content_length - parent_length != sizeof(content) - 1 ||
      std::memcmp(path + parent_length, content, sizeof(content) - 1))
  {
    ERROR_LOG(DISCIO, "hif_000000.nfs is not in a directory named 'content': %s", path);
    return false;
  }

  static const char key_name[] = "code/htk.bin";
  char* const key_path = static_cast<char*>(std::malloc(parent_length + sizeof(key_name)));
  if (!key_path)
    return false;
  std::memcpy(key_path, path, parent_length);
  std::memcpy(key_path + parent_length, key_name, sizeof(key_name));
  File::DirectIOFile key_file(key_path, File::AccessMode::Read);
  const bool read = key_file.Read(key_out, 16);
  if (!read)
    ERROR_LOG(DISCIO, "Failed to read from %s", key_path);
  std::free(key_path);
  return read;
}

void NFSFileReader::GetLBARanges(const NFSHeader& header)
{
  static_assert(ARRAY_SIZE(m_lba_ranges) == ARRAY_SIZE(header.lba_ranges));
  const size_t stored_count = Common::swap32(header.lba_range_count);
  m_lba_range_count =
      stored_count < ARRAY_SIZE(header.lba_ranges) ? stored_count : ARRAY_SIZE(header.lba_ranges);

  for (size_t i = 0; i < m_lba_range_count; ++i)
  {
    const NFSLBARange& unswapped_lba_range = header.lba_ranges[i];
    m_lba_ranges[i].start_block = Common::swap32(unswapped_lba_range.start_block);
    m_lba_ranges[i].num_blocks = Common::swap32(unswapped_lba_range.num_blocks);
  }
}

bool NFSFileReader::OpenFiles(const char* path, size_t directory_length,
                              File::DirectIOFile first_file, u64 expected_raw_size)
{
  const int directory_print_length = static_cast<int>(directory_length);
  const u64 file_count = Common::AlignUp(expected_raw_size, MAX_FILE_SIZE) / MAX_FILE_SIZE;
  if (file_count > MAX_FILES)
  {
    ERROR_LOG(DISCIO, "%.*s needs %" PRIu64 " NFS files, more than %zu", directory_print_length,
              path, file_count, MAX_FILES);
    return false;
  }

  m_raw_size = first_file.GetSize();
  m_files[0] = std::move(first_file);
  m_file_count = 1;

  // the directory, then each file's name
  static const size_t CHILD_NAME_SIZE = 32;
  char* const child_path = static_cast<char*>(std::malloc(directory_length + CHILD_NAME_SIZE));
  if (!child_path)
    return false;
  std::memcpy(child_path, path, directory_length);
  for (u64 i = 1; i < file_count; ++i)
  {
    snprintf(child_path + directory_length, CHILD_NAME_SIZE, "hif_%06" PRIu64 ".nfs", i);
    File::DirectIOFile child(child_path, File::AccessMode::Read);
    if (!child.IsOpen())
    {
      ERROR_LOG(DISCIO, "Failed to open %s", child_path);
      std::free(child_path);
      return false;
    }

    m_raw_size += child.GetSize();
    m_files[m_file_count++] = std::move(child);
  }
  std::free(child_path);

  if (m_raw_size < expected_raw_size)
  {
    ERROR_LOG(DISCIO,
              "Expected sum of NFS file sizes for %.*s to be at least %" PRIu64
              " bytes, but it was %" PRIu64 " bytes",
              directory_print_length, path, expected_raw_size, m_raw_size);
    return false;
  }

  return true;
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

NFSFileReader* NFSFileReader::Create(File::DirectIOFile first_file, const char* path)
{
  const size_t directory_length = NameStart(path, std::strlen(path));
  if (std::strcmp(path + directory_length, "hif_000000.nfs"))
    return nullptr;

  u8 key[16];
  if (!ReadKey(path, directory_length, key))
    return nullptr;

  NFSHeader header;
  if (!first_file.OffsetRead(0, reinterpret_cast<u8*>(&header), sizeof(header)) || header.magic != NFS_MAGIC)
    return nullptr;

  NFSFileReader* const reader = new NFSFileReader;
  reader->GetLBARanges(header);

  const u64 expected_raw_size =
      CalculateExpectedRawSize(reader->m_lba_ranges, reader->m_lba_range_count);

  if (!reader->OpenFiles(path, directory_length, std::move(first_file), expected_raw_size))
  {
    DestroyBlobReader(reader);
    return nullptr;
  }

  reader->SetKey(key);
  return reader;
}

void NFSFileReader::SetKey(const u8* key)
{
  std::memcpy(m_key, key, sizeof(m_key));
  aes_init(&m_aes_context, m_key, sizeof(m_key));
  m_data_size = CalculateExpectedDataSize(m_lba_ranges, m_lba_range_count);
}

BlobReader* NFSFileReader::CopyReader() const
{
  NFSFileReader* const reader = new NFSFileReader;
  std::memcpy(reader->m_lba_ranges, m_lba_ranges, sizeof(m_lba_ranges));
  reader->m_lba_range_count = m_lba_range_count;
  for (size_t i = 0; i < m_file_count; i++)
    reader->m_files[i] = m_files[i];
  reader->m_file_count = m_file_count;
  reader->m_raw_size = m_raw_size;
  reader->SetKey(m_key);
  return reader;
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

  for (size_t i = 0; i < m_lba_range_count; i++)
  {
    const NFSLBARange& range = m_lba_ranges[i];
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

    if (file_index + 1 >= m_file_count)
      return false;
    File::DirectIOFile& file_1 = m_files[file_index];
    File::DirectIOFile& file_2 = m_files[file_index + 1];

    if (!file_1.OffsetRead(offset_in_file, m_current_block_encrypted, PART_1_SIZE))
      return false;

    if (!file_2.OffsetRead(0, m_current_block_encrypted + PART_1_SIZE, PART_2_SIZE))
      return false;
  }
  else
  {
    // Normal case. The read is offset by 0x200 bytes, but it's all within one file.

    if (file_index >= m_file_count)
      return false;
    File::DirectIOFile& file = m_files[file_index];

    if (!file.OffsetRead(offset_in_file, m_current_block_encrypted, BLOCK_SIZE))
    {
      return false;
    }
  }

  return true;
}

void NFSFileReader::DecryptBlock(u64 logical_block_index)
{
  u8 iv[16]{};
  const u64 swapped_block_index = Common::swap64(logical_block_index);
  std::memcpy(iv + ARRAY_SIZE(iv) - sizeof(swapped_block_index), &swapped_block_index,
              sizeof(swapped_block_index));

  aes_cbc_decrypt(&m_aes_context, iv, m_current_block_encrypted, m_current_block_decrypted,
                  BLOCK_SIZE);
}

bool NFSFileReader::ReadAndDecryptBlock(u64 logical_block_index)
{
  const u64 physical_block_index = ToPhysicalBlockIndex(logical_block_index);

  if (physical_block_index == std::numeric_limits<u64>::max())
  {
    // The block isn't physically present. Treat its contents as all zeroes.
    std::memset(m_current_block_decrypted, 0, sizeof(m_current_block_decrypted));
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
    std::memcpy(out_ptr, m_current_block_decrypted + offset_in_block, bytes_to_copy);

    offset += bytes_to_copy;
    nbytes -= bytes_to_copy;
    out_ptr += bytes_to_copy;
  }

  return true;
}

}  // namespace DiscIO
