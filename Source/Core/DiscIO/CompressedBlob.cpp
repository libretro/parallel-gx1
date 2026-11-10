// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/CompressedBlob.h"

#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <encodings/deflate.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
static constexpr u64 uncompressed_flag = 1ULL << 63;

bool IsGCZBlob(File::DirectIOFile& file);

CompressedBlobReader::CompressedBlobReader(File::DirectIOFile file, std::string filename)
    : m_file(std::move(file)), m_file_name(std::move(filename))
{
  m_valid = Initialize();
}

bool CompressedBlobReader::Initialize()
{
  m_file_size = m_file.GetSize();
  m_file.Seek(0, File::SeekOrigin::Begin);
  if (!m_file.Read(reinterpret_cast<u8*>(&m_header), sizeof(m_header)))
    return false;

  if (m_header.magic_cookie != GCZ_MAGIC)
    return false;

  size_t block_pointers_size = m_header.num_blocks * sizeof(u64);
  size_t hashes_size = m_header.num_blocks * sizeof(u32);

  size_t header_size = sizeof(CompressedBlobHeader) + block_pointers_size + hashes_size;

  // Basic sanity check for size before we start allocating
  if (header_size > m_file_size)
  {
    ERROR_LOG_FMT(DISCIO, "Headers' size is larger than file size");
    return false;
  }

  if ((header_size + m_header.compressed_data_size) > m_file_size)
  {
    ERROR_LOG_FMT(DISCIO, "Data size is larger than file size.");
    return false;
  }

  if (m_header.num_blocks == 0)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ file has zero blocks");
    return false;
  }

  // cache block pointers and hashes
  m_block_pointers.resize(m_header.num_blocks);
  if (!m_file.Read(reinterpret_cast<u8*>(m_block_pointers.data()), m_block_pointers.size() * sizeof(m_block_pointers[0])))
    return false;

  m_hashes.resize(m_header.num_blocks);
  if (!m_file.Read(reinterpret_cast<u8*>(m_hashes.data()), m_hashes.size() * sizeof(m_hashes[0])))
    return false;

  m_data_offset = header_size;

  // A compressed block is never ever longer than a decompressed block, so just header.block_size
  // should be fine.
  // I still add some safety margin.
  const u32 zlib_buffer_size = m_header.block_size + 64;
  m_zlib_buffer.resize(zlib_buffer_size);
  if (!(m_inflate = rinflate_new(15)))
    return false;

  SetSectorSize(m_header.block_size);

  return ValidateBlockPointers();
}

std::unique_ptr<CompressedBlobReader> CompressedBlobReader::Create(File::DirectIOFile file,
                                                                   const std::string& filename)
{
  if (IsGCZBlob(file))
  {
    std::unique_ptr<CompressedBlobReader> reader(
        new CompressedBlobReader(std::move(file), filename));

    if (reader->m_valid)
      return reader;
  }

  return nullptr;
}

CompressedBlobReader::~CompressedBlobReader()
{
  rinflate_free(m_inflate);
}

std::unique_ptr<BlobReader> CompressedBlobReader::CopyReader() const
{
  return Create(m_file, m_file_name);
}

// IMPORTANT: Calling this function invalidates all earlier pointers gotten from this function.
u64 CompressedBlobReader::GetBlockCompressedSize(u64 block_num) const
{
  u64 start = m_block_pointers[block_num] & ~uncompressed_flag;
  if (block_num < m_header.num_blocks - 1)
    return (m_block_pointers[block_num + 1] & ~uncompressed_flag) - start;
  else if (block_num == m_header.num_blocks - 1)
    return m_header.compressed_data_size - start;
  else
    ERROR_LOG_FMT(DISCIO, "{} - illegal block number {}", __func__, block_num);
  return 0;
}

bool CompressedBlobReader::GetBlock(u64 block_num, u8* out_ptr)
{
  if (block_num >= m_header.num_blocks)
    return false;

  bool uncompressed = false;
  u64 read_size = GetBlockCompressedSize(block_num);
  u64 offset = m_block_pointers[block_num] + m_data_offset;

  if (offset & uncompressed_flag)
  {
    if (read_size != m_header.block_size)
    {
      ERROR_LOG_FMT(DISCIO, "Uncompressed block with wrong size");
      return false;
    }
    uncompressed = true;
    offset &= ~uncompressed_flag;
  }
  else
  {
    if (read_size > m_zlib_buffer.size())
    {
      ERROR_LOG_FMT(DISCIO, "Compressed block is too large");
      return false;
    }
  }

  if (!m_file.OffsetRead(offset, m_zlib_buffer.data(), read_size))
  {
    ERROR_LOG_FMT(DISCIO, "The disc image \"{}\" is truncated, some of the data is missing.",
                  m_file_name);
    return false;
  }

  // First, check hash.
  const u32 block_hash = common_hash_adler32(m_zlib_buffer.data(), read_size);
  if (block_hash != m_hashes[block_num])
  {
    ERROR_LOG_FMT(DISCIO,
                  "The disc image \"{}\" is corrupt.\n"
                  "Hash of block {} is {:08x} instead of {:08x}.",
                  m_file_name, block_num, block_hash, m_hashes[block_num]);
  }

  if (uncompressed)
  {
    std::memcpy(out_ptr, m_zlib_buffer.data(), m_header.block_size);
  }
  else
  {
    size_t read, wrote, uncomp_size = 0;
    int status;
    if (read_size > m_header.block_size)
    {
      ERROR_LOG_FMT(DISCIO, "Compressed block size is larger than uncompressed block size");
    }
    rinflate_reset(m_inflate, 15);
    rinflate_set_in(m_inflate, m_zlib_buffer.data(), read_size);
    rinflate_set_out(m_inflate, out_ptr, m_header.block_size);
    do
    {
      read = wrote = 0;
      status = rinflate_process(m_inflate, &read, &wrote);
      uncomp_size += wrote;
    } while (status == RDEFLATE_PROCESS_NEXT && (read || wrote));
    if (status != RDEFLATE_PROCESS_END)
    {
      ERROR_LOG_FMT(DISCIO, "Failure reading block {} - out of data and not at end.", block_num);
    }
    if (uncomp_size != m_header.block_size)
    {
      ERROR_LOG_FMT(DISCIO, "Wrong block size");
      return false;
    }
  }
  return true;
}

bool CompressedBlobReader::ValidateBlockPointers() const
{
  size_t valid_pointers = 0;

  // Validate block pointers
  for (u32 i = 0; i < m_header.num_blocks; ++i)
  {
    u64 next;
    if (i + 1 < m_header.num_blocks)
      next = m_block_pointers[i + 1] & ~uncompressed_flag;
    else
      next = m_header.compressed_data_size;

    if (next > m_header.compressed_data_size)
      continue;

    u64 offset = m_block_pointers[i] & ~uncompressed_flag;
    if (offset > m_header.compressed_data_size)
      continue;

    bool uncompressed = m_block_pointers[i] & uncompressed_flag;
    u64 size = next - offset;

    if (uncompressed && size != m_header.block_size)
      continue;

    if (!uncompressed && size > m_zlib_buffer.size())
      continue;

    valid_pointers++;
  }

  size_t invalid_pointers = m_header.num_blocks - valid_pointers;

  if (invalid_pointers > 0)
    ERROR_LOG_FMT(DISCIO, "GCZ file has {} invalid block pointers", invalid_pointers);

  return invalid_pointers == 0;
}

bool IsGCZBlob(File::DirectIOFile& file)
{
  CompressedBlobHeader header;
  return file.OffsetRead(0, reinterpret_cast<u8*>(&header), sizeof(header)) && header.magic_cookie == GCZ_MAGIC;
}

}  // namespace DiscIO
