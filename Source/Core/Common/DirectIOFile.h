// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace File
{
enum class AccessMode
{
  Read,
  Write,
  ReadAndWrite,
};

enum class OpenMode
{
  // Based on the provided `AccessMode`.
  // Read: Existing.
  // Write: Truncate.
  // ReadAndWrite: Existing.
  Default,

  // Either create a new file or open an existing file.
  Always,

  // Like `Always`, but also erase the contents of an existing file.
  Truncate,

  // Require a file to already exist. Fail otherwise.
  Existing,

  // Require a new file be created. Fail if one already exists.
  Create,
};

// A file read and written at offsets of its own keeping, through
// libretro-common's filestream. A file opened for reading alone is
// memory-mapped where the platform can: OffsetRead() then copies straight
// out of the mapping, View() lends from it without copying, and both are
// thread safe on one object. Otherwise one object is one thread's; copies
// open handles of their own.
class DirectIOFile final
{
public:
  DirectIOFile();
  ~DirectIOFile();

  DirectIOFile(const DirectIOFile&);
  DirectIOFile& operator=(const DirectIOFile&);
  DirectIOFile(DirectIOFile&&);
  DirectIOFile& operator=(DirectIOFile&&);

  explicit DirectIOFile(const std::string& path, AccessMode access_mode,
                        OpenMode open_mode = OpenMode::Default);

  bool Open(const std::string& path, AccessMode access_mode,
            OpenMode open_mode = OpenMode::Default);

  bool Close();

  bool IsOpen() const { return m_file != nullptr; }

  // These explicit offset versions entirely ignore the current file position.
  bool OffsetRead(u64 offset, u8* out_ptr, u64 size);
  bool OffsetRead(u64 offset, std::span<u8> out_data)
  {
    return OffsetRead(offset, out_data.data(), out_data.size());
  }
  bool OffsetWrite(u64 offset, const u8* in_ptr, u64 size);
  bool OffsetWrite(auto offset, std::span<const u8> in_data)
  {
    return OffsetWrite(offset, in_data.data(), in_data.size());
  }

  // size octets at offset, lent from the mapping: nullptr when the file is not
  // mapped or the range runs past its end. Valid until the file is closed.
  const u8* View(u64 offset, u64 size) const
  {
    return (m_map && offset <= m_map_size && size <= m_map_size - offset) ? m_map + offset :
                                                                            nullptr;
  }

  // Asks the OS to start reading [offset, offset + size) in; returns at once.
  void Prefetch(u64 offset, u64 size) const;

  // These Read/Write functions advance the current position on success.

  bool Read(u8* out_ptr, u64 size)
  {
    if (!OffsetRead(m_current_offset, out_ptr, size))
      return false;
    m_current_offset += size;
    return true;
  }
  bool Read(std::span<u8> out_data) { return Read(out_data.data(), out_data.size()); }

  bool Write(const u8* in_ptr, u64 size)
  {
    if (!OffsetWrite(m_current_offset, in_ptr, size))
      return false;
    m_current_offset += size;
    return true;
  }
  bool Write(std::span<const u8> in_data) { return Write(in_data.data(), in_data.size()); }

  // Returns 0 on error.
  u64 GetSize() const;

  bool Seek(s64 offset, SeekOrigin origin);

  // Returns 0 when not open.
  u64 Tell() const { return m_current_offset; }

  bool Flush();

private:
  void Swap(DirectIOFile& other);
  DirectIOFile Duplicate() const;

  std::string m_path;
  RFILE* m_file = nullptr;
  const u8* m_map = nullptr;
  u64 m_map_size = 0;
  u64 m_current_offset = 0;
  unsigned m_mode = 0;

  friend bool Resize(DirectIOFile& file, u64 size);
};

bool Resize(DirectIOFile& file, u64 size);

// Attempts to replace a destination file if one already exists.
bool Rename(DirectIOFile& file, const std::string& source_path,
            const std::string& destination_path);
bool Delete(DirectIOFile& file, const std::string& filename);

}  // namespace File
