// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <cstdlib>
#include <string>
#include <cstdint>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Version.h"

#include <streams/file_stream.h>

// On disk format:
// header{
// u32 'DCAC';
// u32 version;  // svn_rev
// u16 sizeof(key_type);
// u16 sizeof(value_type);
//}

// key_value_pair{
// u32 value_size;
// key_type   key;
// value_type[value_size]   value;
//}

namespace Common
{
template <typename K, typename V>
class LinearDiskCacheReader
{
public:
  virtual void Read(const K& key, const V* value, u32 value_size) = 0;
};

// Dead simple unsorted key-value store with append functionality.
// Appended entries are kept in memory, and written to the file in one write when it is synced or
// closed: whoever appends, while a game runs, never touches the disk.
// No random read functionality, all reading is done in OpenAndRead.
// Keys and values can contain any characters, including \0.
//
// Suitable for caching generated shader bytecode between executions.
// Not tuned for extreme performance but should be reasonably fast.
// Does not support keys or values larger than 2GB, which should be reasonable.
// Keys must have non-zero length; values can have zero length.

// K and V are some POD type
// K : the key type
// V : value array type
template <typename K, typename V>
class LinearDiskCache
{
public:
  // return number of read entries
  u32 OpenAndRead(const std::string& filename, LinearDiskCacheReader<K, V>& reader)
  {
    // Since we're reading/writing directly to the storage of K instances,
    // K must be trivially copyable.
    static_assert(std::is_trivially_copyable<K>::value, "K must be a trivially copyable type");

    // close any currently opened file
    Close();
    m_num_entries = 0;
    m_header.Init();

    u64 valid_end = 0;
    {
      // Entries are walked where they lie in the mapped file and values handed to the
      // reader from there, copied only when misaligned for V or when the file is not mapped.
      File::IOFile in(filename, "rb");
      u64 size = 0;
      const u8* map = in.GetMappedPtr(&size);
      u8* owned = nullptr;
      if (in && !map && (size = in.GetSize()) != 0 &&
          (owned = static_cast<u8*>(std::malloc(size_t(size)))) != nullptr &&
          in.ReadBytes(owned, size_t(size)))
      {
        map = owned;
      }
      if (map && size >= sizeof(Header) && !std::memcmp(&m_header, map, sizeof(Header)))
        valid_end = ReadEntries(map, size, reader);
      std::free(owned);
    }

    if (valid_end)
    {
      // appends go after the last whole entry
      m_file.Open(filename, "r+b");
      if (m_file.IsOpen() && m_file.Seek(valid_end, File::SeekOrigin::Begin))
        return m_num_entries;
      m_num_entries = 0;
    }

    // failed to open file for reading or bad header
    // close and recreate file
    Close();
    m_file.Open(filename, "wb");
    WriteHeader();
    return 0;
  }

  // A store of one entry, built in place: the bytes it takes for value_size values, and where in
  // them the value goes, for its producer to write it there.
  static size_t SingleSize(u32 value_size)
  {
    return sizeof(Header) + sizeof(u32) + sizeof(K) + size_t(value_size) * sizeof(V) +
           sizeof(u32);
  }
  static V* SingleValue(u8* store)
  {
    return reinterpret_cast<V*>(store + sizeof(Header) + sizeof(u32) + sizeof(K));
  }

  // Completes the store in store, its value_size values written at SingleValue, and replaces
  // the file with it, atomically, unless it already holds exactly that.
  static bool WriteSingle(const std::string& filename, const K& key, u8* store, u32 value_size)
  {
    LinearDiskCache cache;
    const size_t value_bytes = size_t(value_size) * sizeof(V);
    const size_t size = SingleSize(value_size);
    const u32 entry_number = 1;
    u8* p = store;

    cache.m_header.Init();
    std::memcpy(p, &cache.m_header, sizeof(Header));
    p += sizeof(Header);
    std::memcpy(p, &value_size, sizeof(u32));
    std::memcpy(p + sizeof(u32), &key, sizeof(K));
    std::memcpy(p + sizeof(u32) + sizeof(K) + value_bytes, &entry_number, sizeof(u32));
    return filestream_matches_buf(filename.c_str(), store, size) ||
           filestream_write_file_atomic(filename.c_str(), store, s64(size));
  }

  LinearDiskCache() = default;
  ~LinearDiskCache()
  {
    Close();
    std::free(m_append_buffer);
  }
  LinearDiskCache(const LinearDiskCache&) = delete;
  LinearDiskCache& operator=(const LinearDiskCache&) = delete;

  // Writes the entries appended since, in one write.
  void Sync()
  {
    if (m_append_size && m_file.IsOpen())
      m_file.WriteBytes(m_append_buffer, m_append_size);
    m_append_size = 0;
    m_file.Flush();
  }
  void Close()
  {
    if (m_file.IsOpen())
    {
      Sync();
      m_file.Close();
    }
    m_append_size = 0;
  }

  // Room for an entry of up to max_values values at the end of the store, where its producer
  // writes the value; null when there is none. AppendEnd adds it, or nothing does.
  V* AppendBegin(size_t max_values)
  {
    const size_t entry_bytes = sizeof(u32) + sizeof(K) + max_values * sizeof(V) + sizeof(u32);

    if (!m_file.IsOpen())
      return nullptr;
    if (m_append_capacity - m_append_size < entry_bytes)
    {
      // doubles, so a run allocates a handful of times
      size_t capacity = m_append_capacity ? m_append_capacity * 2 : 64 * 1024;
      while (capacity - m_append_size < entry_bytes)
        capacity *= 2;
      u8* const grown = static_cast<u8*>(std::realloc(m_append_buffer, capacity));
      if (!grown)
        return nullptr;
      m_append_buffer = grown;
      m_append_capacity = capacity;
    }
    return reinterpret_cast<V*>(m_append_buffer + m_append_size + sizeof(u32) + sizeof(K));
  }

  // Adds the entry AppendBegin made room for, with the value_size values written there, to be
  // written with the rest at the next Sync or Close.
  void AppendEnd(const K& key, u32 value_size)
  {
    u8* const out = m_append_buffer + m_append_size;
    const size_t value_bytes = size_t(value_size) * sizeof(V);

    m_num_entries++;
    std::memcpy(out, &value_size, sizeof(u32));
    std::memcpy(out + sizeof(u32), &key, sizeof(K));
    std::memcpy(out + sizeof(u32) + sizeof(K) + value_bytes, &m_num_entries, sizeof(u32));
    m_append_size += sizeof(u32) + sizeof(K) + value_bytes + sizeof(u32);
  }

  // Appends a key-value pair, copied: for a value whose owner does not outlive the store's next
  // Sync or Close.
  void Append(const K& key, const V* value, u32 value_size)
  {
    V* const out = AppendBegin(value_size);
    if (!out)
      return;
    if (value_size)
      std::memcpy(out, value, size_t(value_size) * sizeof(V));
    AppendEnd(key, value_size);
  }

private:
  // Hands each whole entry of [base, base + size) after the header to the reader; returns
  // the offset where the last whole one ends.
  u64 ReadEntries(const u8* base, u64 size, LinearDiskCacheReader<K, V>& reader)
  {
    V* aligned = nullptr;
    u32 aligned_capacity = 0;
    u64 pos = sizeof(Header);

    for (;;)
    {
      u32 value_size;
      u32 entry_number;
      K key;
      if (size - pos < sizeof(u32) + sizeof(K))
        break;
      std::memcpy(&value_size, base + pos, sizeof(u32));
      const u64 value_bytes = u64(value_size) * sizeof(V);
      if (size - pos - sizeof(u32) - sizeof(K) < value_bytes + sizeof(u32))
        break;
      std::memcpy(&key, base + pos + sizeof(u32), sizeof(K));
      const u8* value = base + pos + sizeof(u32) + sizeof(K);
      std::memcpy(&entry_number, value + value_bytes, sizeof(u32));
      if (entry_number != m_num_entries + 1)
        break;

      if (reinterpret_cast<uintptr_t>(value) % alignof(V) != 0)
      {
        if (aligned_capacity < value_size)
        {
          V* const grown = static_cast<V*>(std::realloc(aligned, size_t(value_bytes)));
          if (!grown)
            break;
          aligned = grown;
          aligned_capacity = value_size;
        }
        std::memcpy(aligned, value, size_t(value_bytes));
        value = reinterpret_cast<const u8*>(aligned);
      }
      reader.Read(key, reinterpret_cast<const V*>(value), value_size);

      m_num_entries++;
      pos += sizeof(u32) + sizeof(K) + value_bytes + sizeof(u32);
    }
    std::free(aligned);
    return pos;
  }

  void WriteHeader() { m_file.WriteArray(&m_header, 1); }
  struct Header
  {
    void Init()
    {
      // Null-terminator is intentionally not copied.
      std::memcpy(&id, "DCAC", sizeof(u32));
      const std::string& rev = Common::GetScmRevGitStr();
      std::memcpy(ver, rev.c_str(), rev.size() < sizeof(ver) ? rev.size() : sizeof(ver));
    }

    u32 id = 0;
    const u16 key_t_size = sizeof(K);
    const u16 value_t_size = sizeof(V);
    char ver[40] = {};

  } m_header;

  File::IOFile m_file;
  u8* m_append_buffer = nullptr;
  size_t m_append_capacity = 0;
  size_t m_append_size = 0;
  u32 m_num_entries = 0;
};
}  // namespace Common
