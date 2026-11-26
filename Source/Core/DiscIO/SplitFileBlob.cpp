// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/SplitFileBlob.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace DiscIO
{
SplitPlainFileReader* SplitPlainFileReader::Create(const char* first_file_path)
{
  static const char part0_iso[] = ".part0.iso";
  const size_t length = std::strlen(first_file_path);
  if (length < sizeof(part0_iso) - 1 ||
      std::memcmp(first_file_path + length - (sizeof(part0_iso) - 1), part0_iso,
                  sizeof(part0_iso) - 1))
  {
    return nullptr;
  }

  // the path without ".part0.iso", then each part's ending
  static const size_t PART_NAME_SIZE = 40;
  const size_t base_length = length - (sizeof(part0_iso) - 1);
  char* const part_path = static_cast<char*>(std::malloc(base_length + PART_NAME_SIZE));
  if (!part_path)
    return nullptr;
  std::memcpy(part_path, first_file_path, base_length);

  SplitPlainFileReader* const reader = new SplitPlainFileReader;
  size_t index = 0;
  u64 offset = 0;
  while (index < MAX_FILES)
  {
    snprintf(part_path + base_length, PART_NAME_SIZE, ".part%zu.iso", index);
    File::DirectIOFile f(part_path, File::AccessMode::Read);
    if (!f.IsOpen())
      break;
    const u64 size = f.GetSize();
    if (size == 0)
    {
      std::free(part_path);
      DestroyBlobReader(reader);
      return nullptr;
    }
    SingleFile& file = reader->m_files[reader->m_file_count++];
    file.file = std::move(f);
    file.offset = offset;
    file.size = size;
    offset += size;
    ++index;
  }
  std::free(part_path);

  if (reader->m_file_count < 2)
  {
    DestroyBlobReader(reader);
    return nullptr;
  }

  reader->m_size = offset;
  return reader;
}

BlobReader* SplitPlainFileReader::CopyReader() const
{
  SplitPlainFileReader* const reader = new SplitPlainFileReader;
  for (size_t i = 0; i < m_file_count; i++)
    reader->m_files[i] = m_files[i];
  reader->m_file_count = m_file_count;
  reader->m_size = m_size;
  return reader;
}

bool SplitPlainFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (offset >= m_size)
    return false;

  u64 current_offset = offset;
  u64 rest = nbytes;
  u8* out = out_ptr;
  for (size_t i = 0; i < m_file_count; i++)
  {
    SingleFile& file = m_files[i];
    if (current_offset >= file.offset && current_offset < file.offset + file.size)
    {
      auto& f = file.file;
      const u64 offset_in_file = current_offset - file.offset;
      const u64 file_left = file.size - offset_in_file;
      const u64 current_read = file_left < rest ? file_left : rest;
      if (!f.OffsetRead(offset_in_file, out, current_read))
        return false;

      rest -= current_read;
      if (rest == 0)
        return true;
      current_offset += current_read;
      out += current_read;
    }
  }

  return rest == 0;
}
}  // namespace DiscIO
