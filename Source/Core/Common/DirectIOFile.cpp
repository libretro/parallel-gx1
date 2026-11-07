// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/DirectIOFile.h"

#include <cstring>
#include <utility>

#include <streams/file_stream.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"

namespace File
{
DirectIOFile::DirectIOFile() = default;

DirectIOFile::~DirectIOFile()
{
  Close();
}

DirectIOFile::DirectIOFile(const DirectIOFile& other)
{
  *this = other.Duplicate();
}

DirectIOFile& DirectIOFile::operator=(const DirectIOFile& other)
{
  return *this = other.Duplicate();
}

DirectIOFile::DirectIOFile(DirectIOFile&& other)
{
  Swap(other);
}

DirectIOFile& DirectIOFile::operator=(DirectIOFile&& other)
{
  Close();
  Swap(other);
  return *this;
}

DirectIOFile::DirectIOFile(const std::string& path, AccessMode access_mode, OpenMode open_mode)
{
  Open(path, access_mode, open_mode);
}

bool DirectIOFile::Open(const std::string& path, AccessMode access_mode, OpenMode open_mode)
{
  unsigned mode;
  int64_t map_len = 0;

  ASSERT(!IsOpen());

  if (open_mode == OpenMode::Default)
    open_mode = (access_mode == AccessMode::Write) ? OpenMode::Truncate : OpenMode::Existing;

  // This is not a sensible combination. Fail here to not rely on OS-specific behaviors.
  if (access_mode == AccessMode::Read && open_mode == OpenMode::Truncate)
    return false;

  if (open_mode == OpenMode::Existing || open_mode == OpenMode::Create)
  {
    const bool exists = filestream_exists(path.c_str());
    if (exists != (open_mode == OpenMode::Existing))
      return false;
  }

  if (access_mode == AccessMode::Read)
    mode = RETRO_VFS_FILE_ACCESS_READ;
  else if (access_mode == AccessMode::Write)
    mode = RETRO_VFS_FILE_ACCESS_WRITE;
  else
    mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
  if (open_mode != OpenMode::Truncate && mode != RETRO_VFS_FILE_ACCESS_READ)
    mode |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

  m_file = filestream_open(path.c_str(), mode,
                           mode == RETRO_VFS_FILE_ACCESS_READ ?
                               RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS :
                               RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if (!m_file)
    return false;

  m_path = path;
  m_mode = mode;
  m_current_offset = 0;
  m_map = filestream_get_mapped_ptr(m_file, &map_len);
  m_map_size = m_map ? u64(map_len) : 0;
  return true;
}

bool DirectIOFile::Close()
{
  RFILE* file = m_file;
  if (!file)
    return false;
  m_file = nullptr;
  m_map = nullptr;
  m_map_size = 0;
  m_current_offset = 0;
  return filestream_close(file) == 0;
}

bool DirectIOFile::OffsetRead(u64 offset, u8* out_ptr, u64 size)
{
  const u8* src = View(offset, size);
  if (src)
  {
    std::memcpy(out_ptr, src, size);
    return true;
  }
  if (!m_file || m_map)
    return false;
  return filestream_seek(m_file, s64(offset), RETRO_VFS_SEEK_POSITION_START) >= 0 &&
         filestream_read(m_file, out_ptr, s64(size)) == s64(size);
}

bool DirectIOFile::OffsetWrite(u64 offset, const u8* in_ptr, u64 size)
{
  if (!m_file)
    return false;
  return filestream_seek(m_file, s64(offset), RETRO_VFS_SEEK_POSITION_START) >= 0 &&
         filestream_write(m_file, in_ptr, s64(size)) == s64(size);
}

void DirectIOFile::Prefetch(u64 offset, u64 size) const
{
  if (m_file)
    filestream_prefetch(m_file, offset, size);
}

u64 DirectIOFile::GetSize() const
{
  int64_t size;
  if (m_map)
    return m_map_size;
  if (!m_file)
    return 0;
  size = filestream_get_size(m_file);
  return size > 0 ? u64(size) : 0;
}

bool DirectIOFile::Seek(s64 offset, SeekOrigin origin)
{
  if (!IsOpen())
    return false;

  u64 reference_pos = 0;
  switch (origin)
  {
  case SeekOrigin::Current:
    reference_pos = m_current_offset;
    break;
  case SeekOrigin::End:
    reference_pos = GetSize();
    break;
  default:
    break;
  }

  // Don't let our current offset underflow.
  if (offset < 0 && u64(-offset) > reference_pos)
    return false;

  m_current_offset = reference_pos + offset;
  return true;
}

bool DirectIOFile::Flush()
{
  return m_file && filestream_flush(m_file) == 0;
}

void DirectIOFile::Swap(DirectIOFile& other)
{
  std::swap(m_path, other.m_path);
  std::swap(m_file, other.m_file);
  std::swap(m_map, other.m_map);
  std::swap(m_map_size, other.m_map_size);
  std::swap(m_current_offset, other.m_current_offset);
  std::swap(m_mode, other.m_mode);
}

DirectIOFile DirectIOFile::Duplicate() const
{
  DirectIOFile result;
  int64_t map_len = 0;

  if (!IsOpen())
    return result;

  // a handle of its own, never truncating what this one holds
  result.m_file = filestream_open(
      m_path.c_str(),
      m_mode == RETRO_VFS_FILE_ACCESS_READ ? m_mode :
                                             m_mode | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
      m_mode == RETRO_VFS_FILE_ACCESS_READ ? RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS :
                                             RETRO_VFS_FILE_ACCESS_HINT_NONE);
  ASSERT(result.IsOpen());
  if (!result.m_file)
    return result;

  result.m_path = m_path;
  result.m_mode = m_mode;
  result.m_current_offset = m_current_offset;
  result.m_map = filestream_get_mapped_ptr(result.m_file, &map_len);
  result.m_map_size = result.m_map ? u64(map_len) : 0;
  return result;
}

bool Resize(DirectIOFile& file, u64 size)
{
  return file.m_file && filestream_truncate(file.m_file, s64(size)) == 0;
}

bool Rename(DirectIOFile& file, const std::string& source_path,
            const std::string& destination_path)
{
  return file.IsOpen() && Rename(source_path, destination_path);
}

bool Delete(DirectIOFile& file, const std::string& filename)
{
  return file.IsOpen() && Delete(filename, IfAbsentBehavior::NoConsoleWarning);
}

}  // namespace File
