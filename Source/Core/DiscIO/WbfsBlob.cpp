// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WbfsBlob.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

namespace DiscIO
{
static constexpr u64 WII_SECTOR_SIZE = 0x8000;
static constexpr u64 WII_SECTOR_COUNT = 143432 * 2;
static constexpr u64 WII_DISC_HEADER_SIZE = 256;

WbfsFileReader::WbfsFileReader(File::DirectIOFile file, const char* path) : m_size(0), m_good(false)
{
  if (!AddFileToList(std::move(file)))
    return;
  if (path && *path)
    OpenAdditionalFiles(path);
  if (!ReadHeader())
    return;

  // Grab disc info (assume slot 0, checked in ReadHeader())
  m_wlba_table = static_cast<u16*>(std::calloc(m_blocks_per_disc, sizeof(u16)));
  if (!m_wlba_table)
    return;
  m_good = true;
  m_files[0].file.Seek(m_hd_sector_size + WII_DISC_HEADER_SIZE /*+ i * m_disc_info_size*/,
                       File::SeekOrigin::Begin);
  m_files[0].file.Read(reinterpret_cast<u8*>(m_wlba_table), m_blocks_per_disc * sizeof(u16));
  for (size_t i = 0; i < m_blocks_per_disc; i++)
    m_wlba_table[i] = Common::swap16(m_wlba_table[i]);
}

WbfsFileReader::~WbfsFileReader()
{
  std::free(m_wlba_table);
}

BlobReader* WbfsFileReader::CopyReader() const
{
  WbfsFileReader* retval = new WbfsFileReader(m_files[0].file);
  for (size_t ix = 1; ix < m_file_count; ix++)
    retval->AddFileToList(m_files[ix].file);
  return retval;
}

u64 WbfsFileReader::GetDataSize() const
{
  return WII_SECTOR_COUNT * WII_SECTOR_SIZE;
}

void WbfsFileReader::OpenAdditionalFiles(const char* path)
{
  const size_t length = std::strlen(path);
  if (length < 4)
    return;

  ASSERT(m_file_count != 0);  // The code below gives .wbf0 for index 0, but it should be .wbfs

  char* const current_path = static_cast<char*>(std::malloc(length + 1));
  if (!current_path)
    return;
  std::memcpy(current_path, path, length + 1);
  // Replace last character with index (e.g. wbfs = wbf1)
  while (m_file_count < MAX_FILES)
  {
    current_path[length - 1] = static_cast<char>('0' + m_file_count);
    if (!AddFileToList(File::DirectIOFile(current_path, File::AccessMode::Read)))
      break;
  }
  std::free(current_path);
}

bool WbfsFileReader::AddFileToList(File::DirectIOFile file)
{
  if (!file.IsOpen() || m_file_count == MAX_FILES)
    return false;

  const u64 file_size = file.GetSize();
  FileEntry& entry = m_files[m_file_count++];
  entry.file = std::move(file);
  entry.base_address = m_size;
  entry.size = file_size;
  m_size += file_size;

  return true;
}

bool WbfsFileReader::ReadHeader()
{
  // Read hd size info
  m_files[0].file.Seek(0, File::SeekOrigin::Begin);
  m_files[0].file.Read(reinterpret_cast<u8*>(&m_header), sizeof(m_header));
  if (m_header.magic != WBFS_MAGIC)
    return false;

  m_header.hd_sector_count = Common::swap32(m_header.hd_sector_count);
  m_hd_sector_size = 1ull << m_header.hd_sector_shift;

  if (m_size != (m_header.hd_sector_count * m_hd_sector_size))
    return false;

  // Read wbfs cluster info
  m_wbfs_sector_size = 1ull << m_header.wbfs_sector_shift;
  m_wbfs_sector_count = m_size / m_wbfs_sector_size;

  if (m_wbfs_sector_size < WII_SECTOR_SIZE)
    return false;

  m_blocks_per_disc =
      (WII_SECTOR_COUNT * WII_SECTOR_SIZE + m_wbfs_sector_size - 1) / m_wbfs_sector_size;
  m_disc_info_size =
      Common::AlignUp(WII_DISC_HEADER_SIZE + m_blocks_per_disc * sizeof(u16), m_hd_sector_size);

  return m_header.disc_table[0] != 0;
}

bool WbfsFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (offset + nbytes > GetDataSize())
    return false;

  while (nbytes)
  {
    u64 read_size;
    auto& data_file = SeekToCluster(offset, &read_size);
    if (read_size == 0)
      return false;
    if (nbytes < read_size)
      read_size = nbytes;

    if (!data_file.Read(out_ptr, read_size))
    {
      return false;
    }

    out_ptr += read_size;
    nbytes -= read_size;
    offset += read_size;
  }

  return true;
}

File::DirectIOFile& WbfsFileReader::SeekToCluster(u64 offset, u64* available)
{
  u64 base_cluster = (offset >> m_header.wbfs_sector_shift);
  if (base_cluster < m_blocks_per_disc)
  {
    u64 cluster_address = m_wbfs_sector_size * m_wlba_table[base_cluster];
    u64 cluster_offset = offset & (m_wbfs_sector_size - 1);
    u64 final_address = cluster_address + cluster_offset;

    for (size_t f = 0; f < m_file_count; f++)
    {
      FileEntry& file_entry = m_files[f];
      if (final_address < (file_entry.base_address + file_entry.size))
      {
        file_entry.file.Seek(final_address - file_entry.base_address, File::SeekOrigin::Begin);
        if (available)
        {
          u64 till_end_of_file = file_entry.size - (final_address - file_entry.base_address);
          u64 till_end_of_sector = m_wbfs_sector_size - cluster_offset;
          *available =
              till_end_of_file < till_end_of_sector ? till_end_of_file : till_end_of_sector;
        }

        return file_entry.file;
      }
    }
  }

  ERROR_LOG(DISCIO, "Read beyond end of disc");
  if (available)
    *available = 0;
  m_files[0].file.Seek(0, File::SeekOrigin::Begin);
  return m_files[0].file;
}

WbfsFileReader* WbfsFileReader::Create(File::DirectIOFile file, const char* path)
{
  WbfsFileReader* reader = new WbfsFileReader(std::move(file), path);

  if (!reader->IsGood())
  {
    DestroyBlobReader(reader);
    reader = nullptr;
  }

  return reader;
}

}  // namespace DiscIO
