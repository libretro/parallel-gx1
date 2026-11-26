// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Utilities to parse and modify a Wii SYSCONF file and its sections.

#pragma once

#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace IOS::HLE::FS
{
class FileHandle;
class FileSystem;
}  // namespace IOS::HLE::FS

class SysConf final
{
public:
  explicit SysConf(IOS::HLE::FS::FileSystem* fs);
  ~SysConf();
  SysConf(const SysConf&) = delete;
  SysConf& operator=(const SysConf&) = delete;

  void Clear();
  void Load();
  bool Save() const;

  // An entry name is stored as its length - 1 in five bits.
  static constexpr size_t MAX_NAME_LENGTH = 32;

  struct Entry
  {
    enum Type : u8
    {
      BigArray = 1,
      SmallArray = 2,
      Byte = 3,
      Short = 4,
      Long = 5,
      LongLong = 6,
      // Should really be named Bool, but this conflicts with some random macro. :/
      ByteBool = 7,
    };

    // Intended for use with the non array types.
    template <typename T>
    T GetData(T default_value) const
    {
      if (size != sizeof(T))
        return default_value;

      T value;
      std::memcpy(&value, bytes, size);
      return Common::FromBigEndian(value);
    }
    template <typename T>
    void SetData(T value)
    {
      ASSERT(sizeof(value) == size);

      value = Common::FromBigEndian(value);
      std::memcpy(bytes, &value, size);
    }

    Type type;
    char name[MAX_NAME_LENGTH + 1];
    // Owned by the SysConf
    u8* bytes;
    u32 size;
  };

  // Adds an entry holding a copy of the size bytes at data, or zeroes when data is null; a non
  // array entry added without bytes gets zeroes of its type's size. Nullptr when out of memory.
  Entry* AddEntry(Entry::Type type, const char* name, const u8* data, u32 size);
  // Resizes the entry's bytes, zeroing any new ones; false when out of memory.
  static bool ResizeEntry(Entry* entry, u32 size);
  Entry* GetEntry(const char* key);
  const Entry* GetEntry(const char* key) const;
  Entry* GetOrAddEntry(const char* key, Entry::Type type);

  // Intended for use with the non array types.
  template <typename T>
  T GetData(const char* key, T default_value) const
  {
    const Entry* entry = GetEntry(key);
    return entry ? entry->GetData(default_value) : default_value;
  }
  template <typename T>
  void SetData(const char* key, Entry::Type type, T value)
  {
    if (Entry* entry = GetOrAddEntry(key, type))
      entry->SetData(value);
  }

private:
  void InsertDefaultEntries();
  bool LoadFromFile(const IOS::HLE::FS::FileHandle& file);

  // Grown by doubling and kept; adding an entry can move the others.
  Entry* m_entries = nullptr;
  u32 m_entry_count = 0;
  u32 m_entry_capacity = 0;
  IOS::HLE::FS::FileSystem* m_fs;  // not owning
};
