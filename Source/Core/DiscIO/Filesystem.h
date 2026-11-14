// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
class VolumeDisc;

// An entry of a GameCube or Wii disc's file system table (FST), a value copied as one: it holds
// where the entry is, nothing it points at is its own.
class FileInfo
{
public:
  class const_iterator;

  FileInfo() : m_fst(nullptr), m_offset_shift(0), m_index(0), m_total_file_infos(0) {}
  // None of the constructors take ownership of FST pointers

  // Set everything manually
  FileInfo(const u8* fst, u8 offset_shift, u32 index, u32 total_file_infos);
  // For the root object only
  FileInfo(const u8* fst, u8 offset_shift);
  // Copy data that is common to the whole file system
  FileInfo(const FileInfo& file_info, u32 index);
  FileInfo(const FileInfo& file_info) = default;
  FileInfo& operator=(const FileInfo& file_info) = default;

  bool operator==(const FileInfo& other) const { return GetAddress() == other.GetAddress(); }
  const_iterator cbegin() const;
  const_iterator cend() const;
  const_iterator begin() const;
  const_iterator end() const;

  // The offset of a file on the disc (inside the partition, if there is one).
  // Not guaranteed to return a meaningful value for directories.
  u64 GetOffset() const;
  // The size of a file.
  // Not guaranteed to return a meaningful value for directories.
  u32 GetSize() const;
  bool IsRoot() const;
  bool IsDirectory() const;
  std::string GetName() const;
  bool NameCaseInsensitiveEquals(std::string_view other) const;
  // GetPath will find the parents of the current object and call GetName on them,
  // so it's slower than other functions. If you're traversing through folders
  // to get a file and its path, building the path while traversing is faster.
  std::string GetPath() const;

  bool IsValid(u64 fst_size, const FileInfo& parent_directory) const;

  // The entry after this one (after its contents, for a directory), as iterators step
  FileInfo& operator++();

private:
  // Only used for comparisons with other file info objects
  uintptr_t GetAddress() const;

  enum class EntryProperty
  {
    // NAME_OFFSET's lower 3 bytes are the name's offset within the name table.
    // NAME_OFFSET's upper 1 byte is 1 for directories and 0 for files.
    NAME_OFFSET = 0,
    // For files, FILE_OFFSET is the file offset in the partition,
    // and for directories, it's the FST index of the parent directory.
    // The root directory has its parent directory index set to 0.
    FILE_OFFSET = 1,
    // For files, FILE_SIZE is the file size, and for directories,
    // it's the FST index of the next entry that isn't in the directory.
    FILE_SIZE = 2
  };

  // For files, returns the index of the next item. For directories,
  // returns the index of the next item that isn't inside it.
  u32 GetNextIndex() const;
  // Returns one of the three properties of this FST entry.
  // Read the comments in EntryProperty for details.
  u32 Get(EntryProperty entry_property) const;
  // Returns the name offset, excluding the directory identification byte
  u64 GetNameOffset() const;

  const u8* m_fst;
  u8 m_offset_shift;
  u32 m_index;
  u32 m_total_file_infos;
};

// Walks the entries of a directory, holding the one it is at
class FileInfo::const_iterator
{
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = const FileInfo;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;

  const_iterator() : m_valid(false) {}
  explicit const_iterator(const FileInfo& file_info) : m_file_info(file_info), m_valid(true) {}

  const_iterator& operator++()
  {
    ++m_file_info;
    return *this;
  }
  const_iterator operator++(int)
  {
    const_iterator old = *this;
    ++m_file_info;
    return old;
  }
  bool operator==(const const_iterator& it) const
  {
    return m_valid ? (it.m_valid && m_file_info == it.m_file_info) : !it.m_valid;
  }
  // Incrementing or destroying an iterator will invalidate its returned references and
  // pointers, but will not invalidate copies of the iterator or file info object.
  const FileInfo& operator*() const { return m_file_info; }
  const FileInfo* operator->() const { return &m_file_info; }

private:
  FileInfo m_file_info;
  bool m_valid;
};

inline FileInfo::const_iterator FileInfo::cbegin() const
{
  return begin();
}

inline FileInfo::const_iterator FileInfo::cend() const
{
  return end();
}

// The file system of a GameCube disc or a Wii partition: its FST, read when made. Movable (the
// entries point into the FST, which moves with it), not copyable.
class FileSystem
{
public:
  // An invalid one
  FileSystem() : m_valid(false) {}
  FileSystem(const VolumeDisc* volume, const Partition& partition);
  FileSystem(FileSystem&& other) = default;
  FileSystem& operator=(FileSystem&& other) = default;
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;

  // If IsValid is false, GetRoot must not be called.
  bool IsValid() const { return m_valid; }
  // The object returned by GetRoot and all objects created from it
  // are only valid for as long as the file system object is valid.
  const FileInfo& GetRoot() const;
  // The file or directory at path into *file_info; false if there is none
  bool FindFileInfo(std::string_view path, FileInfo* file_info) const;
  // The file that holds disc_offset into *file_info; false if there is none
  bool FindFileInfo(u64 disc_offset, FileInfo* file_info) const;

private:
  bool FindFileInfo(std::string_view path, const FileInfo& directory, FileInfo* file_info) const;

  bool m_valid;
  std::vector<u8> m_file_system_table;
  FileInfo m_root;
  // Maps the end offset of files to FST indexes
  mutable std::map<u64, u32> m_offset_file_info_cache;
};

}  // namespace DiscIO
