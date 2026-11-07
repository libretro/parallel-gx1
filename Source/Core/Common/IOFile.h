// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#include "Common/CommonTypes.h"

struct RFILE;

namespace File
{
enum class SeekOrigin
{
  Begin,
  Current,
  End,
};

enum class SharedAccess
{
  Default,
  Read,
};

// A file through libretro-common's filestream: the frontend's VFS where it
// has one, libretro-common's own implementation otherwise.
class IOFile
{
public:
  IOFile();
  IOFile(const char* filename, const char openmode[], SharedAccess sh = SharedAccess::Default);
  IOFile(const std::string& filename, const char openmode[],
         SharedAccess sh = SharedAccess::Default)
      : IOFile(filename.c_str(), openmode, sh)
  {
  }

  ~IOFile();

  IOFile(const IOFile&) = delete;
  IOFile& operator=(const IOFile&) = delete;

  IOFile(IOFile&& other) noexcept;
  IOFile& operator=(IOFile&& other) noexcept;

  void Swap(IOFile& other) noexcept;

  bool Open(const char* filename, const char openmode[], SharedAccess sh = SharedAccess::Default);
  bool Open(const std::string& filename, const char openmode[],
            SharedAccess sh = SharedAccess::Default)
  {
    return Open(filename.c_str(), openmode, sh);
  }
  bool Close();

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool ReadArray(T* elements, size_t count, size_t* num_read = nullptr)
  {
    const size_t read_count = RawRead(elements, count * sizeof(T)) / sizeof(T);
    if (read_count != count)
      m_good = false;
    if (num_read)
      *num_read = read_count;
    return m_good;
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool WriteArray(const T* elements, size_t count)
  {
    if (!RawWrite(elements, count * sizeof(T)))
      m_good = false;
    return m_good;
  }

  template <typename T, std::size_t N>
  bool ReadArray(std::array<T, N>* elements, size_t* num_read = nullptr)
  {
    return ReadArray(elements->data(), elements->size(), num_read);
  }

  template <typename T, std::size_t N>
  bool WriteArray(const std::array<T, N>& elements)
  {
    return WriteArray(elements.data(), elements.size());
  }

  bool ReadBytes(void* data, size_t length) { return ReadArray(static_cast<char*>(data), length); }

  bool WriteBytes(const void* data, size_t length)
  {
    return WriteArray(static_cast<const char*>(data), length);
  }

  bool WriteString(std::string_view str) { return WriteBytes(str.data(), str.size()); }

  // Writes length zero octets.
  bool WriteZeros(size_t length);

  bool IsOpen() const { return m_file != nullptr; }
  // m_good is set to false when a read, write or other function fails
  bool IsGood() const { return m_good; }
  explicit operator bool() const { return IsGood() && IsOpen(); }
  RFILE* GetHandle() { return m_file; }

  // The whole file, read-only, where it is memory-mapped (a file opened for
  // reading by the local implementation); nullptr otherwise. Valid until close.
  const u8* GetMappedPtr(u64* size = nullptr);

  // Up to length octets at the position, lent rather than copied, the position
  // moving past them; nullptr when nothing there can be lent. Valid until the
  // next call on this file.
  const u8* View(size_t length, size_t* lent);

  // Asks the OS to start reading [offset, offset + length) in; returns at once.
  void Prefetch(u64 offset, u64 length);

  bool Seek(s64 offset, SeekOrigin origin);
  u64 Tell() const;
  u64 GetSize() const;
  bool Resize(u64 size);
  bool Flush();

  // clear error state
  void ClearError() { m_good = true; }

private:
  size_t RawRead(void* data, size_t length);
  bool RawWrite(const void* data, size_t length);

  RFILE* m_file;
  bool m_good;
};

// A whole file lent for reading: straight from its mapping where the file
// is mapped, from one malloc'd buffer otherwise. data stays NULL and size 0
// when the file can't be read.
struct FileLoan
{
  IOFile file;
  const u8* data;
  u64 size;
  u8* owned;
};

bool LoanFile(FileLoan* loan, const char* path);
void EndLoan(FileLoan* loan);

}  // namespace File
