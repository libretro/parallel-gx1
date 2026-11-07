// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/IOFile.h"

#include <cstring>
#include <string>

#include <streams/file_stream.h>

#include "Common/CommonTypes.h"

namespace File
{
/* fopen() modes in filestream's terms. "a" positions at the end once,
 * at open: nothing in the tree mixes seeks with appends. */
static unsigned ModeFromString(const char* openmode, bool* append)
{
  bool read = false, write = false, plus = false;
  const char* p;
  *append = false;
  for (p = openmode; *p; p++)
  {
    switch (*p)
    {
    case 'r':
      read = true;
      break;
    case 'w':
      write = true;
      break;
    case 'a':
      *append = true;
      break;
    case '+':
      plus = true;
      break;
    default:
      break;
    }
  }
  if (*append)
    return RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING |
           (plus ? RETRO_VFS_FILE_ACCESS_READ : 0);
  if (read && plus)
    return RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
  if (write)
    return plus ? RETRO_VFS_FILE_ACCESS_READ_WRITE : RETRO_VFS_FILE_ACCESS_WRITE;
  return RETRO_VFS_FILE_ACCESS_READ;
}

IOFile::IOFile() : m_file(nullptr), m_good(true)
{
}

IOFile::IOFile(const char* filename, const char openmode[], SharedAccess sh)
    : m_file(nullptr), m_good(true)
{
  Open(filename, openmode, sh);
}

IOFile::~IOFile()
{
  Close();
}

IOFile::IOFile(IOFile&& other) noexcept : m_file(nullptr), m_good(true)
{
  Swap(other);
}

IOFile& IOFile::operator=(IOFile&& other) noexcept
{
  Swap(other);
  return *this;
}

void IOFile::Swap(IOFile& other) noexcept
{
  RFILE* file = m_file;
  bool good = m_good;
  m_file = other.m_file;
  m_good = other.m_good;
  other.m_file = file;
  other.m_good = good;
}

bool IOFile::Open(const char* filename, const char openmode[], SharedAccess)
{
  bool append;
  unsigned mode;

  Close();
  mode = ModeFromString(openmode, &append);
  /* read-only files are mapped where the platform can: reads copy straight
   * out of the page cache, and GetMappedPtr()/View() lend without copying */
  m_file = filestream_open(filename, mode,
                           mode == RETRO_VFS_FILE_ACCESS_READ ?
                               RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS :
                               RETRO_VFS_FILE_ACCESS_HINT_NONE);
  m_good = m_file != nullptr;
  if (m_good && append && filestream_seek(m_file, 0, RETRO_VFS_SEEK_POSITION_END) < 0)
    m_good = false;
  return m_good;
}

bool IOFile::Close()
{
  if (!m_file || filestream_close(m_file) != 0)
    m_good = false;
  m_file = nullptr;
  return m_good;
}

size_t IOFile::RawRead(void* data, size_t length)
{
  int64_t got;
  if (!m_file)
    return 0;
  if (!length)
    return 0;
  got = filestream_read(m_file, data, (int64_t)length);
  return got > 0 ? (size_t)got : 0;
}

bool IOFile::RawWrite(const void* data, size_t length)
{
  if (!m_file)
    return false;
  if (!length)
    return true;
  return filestream_write(m_file, data, (int64_t)length) == (int64_t)length;
}

bool IOFile::WriteZeros(size_t length)
{
  static const u8 zeros[4096] = {};
  while (length && m_good)
  {
    const size_t n = length < sizeof(zeros) ? length : sizeof(zeros);
    WriteBytes(zeros, n);
    length -= n;
  }
  return m_good;
}

const u8* IOFile::GetMappedPtr(u64* size)
{
  int64_t len = 0;
  const u8* ptr = m_file ? filestream_get_mapped_ptr(m_file, &len) : nullptr;
  if (size)
    *size = ptr ? (u64)len : 0;
  return ptr;
}

const u8* IOFile::View(size_t length, size_t* lent)
{
  const uint8_t* ptr = nullptr;
  const int64_t n = m_file ? filestream_view(m_file, (int64_t)length, &ptr) : -1;
  if (n <= 0)
  {
    *lent = 0;
    return nullptr;
  }
  *lent = (size_t)n;
  return ptr;
}

void IOFile::Prefetch(u64 offset, u64 length)
{
  if (m_file)
    filestream_prefetch(m_file, offset, length);
}

u64 IOFile::GetSize() const
{
  int64_t size;
  if (!m_file)
    return 0;
  size = filestream_get_size(m_file);
  return size > 0 ? (u64)size : 0;
}

bool IOFile::Seek(s64 offset, SeekOrigin origin)
{
  int whence;
  switch (origin)
  {
  case SeekOrigin::Begin:
    whence = RETRO_VFS_SEEK_POSITION_START;
    break;
  case SeekOrigin::Current:
    whence = RETRO_VFS_SEEK_POSITION_CURRENT;
    break;
  case SeekOrigin::End:
    whence = RETRO_VFS_SEEK_POSITION_END;
    break;
  default:
    return false;
  }
  if (!m_file || filestream_seek(m_file, offset, whence) < 0)
    m_good = false;
  return m_good;
}

u64 IOFile::Tell() const
{
  int64_t pos;
  if (!m_file)
    return UINT64_MAX;
  pos = filestream_tell(m_file);
  return pos < 0 ? UINT64_MAX : (u64)pos;
}

bool IOFile::Flush()
{
  if (!m_file || filestream_flush(m_file) != 0)
    m_good = false;
  return m_good;
}

bool IOFile::Resize(u64 size)
{
  if (!m_file || filestream_truncate(m_file, (int64_t)size) != 0)
    m_good = false;
  return m_good;
}

}  // namespace File
