// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <retro_atomic.h>
#include <retro_miscellaneous.h>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace File
{
struct FSTEntry;
}  // namespace File

struct RFILE;

namespace DiscIO
{
enum class PartitionType : u32;

class DirectoryBlobReader;
class VolumeDisc;

// Returns true if the path is inside a DirectoryBlob and doesn't represent the DirectoryBlob itself
bool ShouldHideFromGameList(const std::string& volume_path);

// Bytes shared by the content chunks that refer to them: a host file's path
// (NUL included) or a run of memory. Reference counted; the bytes follow the
// header in the same allocation.
struct ContentBlob
{
  retro_atomic_int_t refs;
  u64 size;
};

// the bytes start here, 16-aligned whatever the atomic's width
#define CONTENT_BLOB_HEADER ((sizeof(ContentBlob) + 15) & ~size_t(15))

// The size octets after the header, uninitialized; one reference.
ContentBlob* ContentBlobAlloc(size_t size);
ContentBlob* ContentBlobCreate(const void* data, size_t size);
void ContentBlobRetain(ContentBlob* blob);
void ContentBlobRelease(ContentBlob* blob);
inline u8* ContentBlobData(ContentBlob* blob)
{
  return reinterpret_cast<u8*>(blob) + CONTENT_BLOB_HEADER;
}
inline const u8* ContentBlobData(const ContentBlob* blob)
{
  return reinterpret_cast<const u8*>(blob) + CONTENT_BLOB_HEADER;
}

enum ContentType : u8
{
  // a host file: blob is its path, offset the file offset of the first byte
  CONTENT_FILE,
  // blob's bytes from offset on
  CONTENT_MEMORY,
  // the reader's own partition, encrypted on the fly: offset into it, and
  // the partition_data_offset passed to EncryptPartitionData()
  CONTENT_PARTITION,
  // fixed_byte repeated
  CONTENT_FIXED_BYTE,
};

// Where a content chunk's bytes come from. Copies share the blob.
struct ContentSource
{
  ContentSource()
      : type(CONTENT_FIXED_BYTE), fixed_byte(0), offset(0), partition_data_offset(0),
        blob(nullptr)
  {
  }
  ContentSource(const ContentSource& other)
      : type(other.type), fixed_byte(other.fixed_byte), offset(other.offset),
        partition_data_offset(other.partition_data_offset), blob(other.blob)
  {
    if (blob)
      ContentBlobRetain(blob);
  }
  ContentSource(ContentSource&& other) noexcept
      : type(other.type), fixed_byte(other.fixed_byte), offset(other.offset),
        partition_data_offset(other.partition_data_offset), blob(other.blob)
  {
    other.blob = nullptr;
  }
  ContentSource& operator=(const ContentSource& other)
  {
    if (other.blob)
      ContentBlobRetain(other.blob);
    if (blob)
      ContentBlobRelease(blob);
    type = other.type;
    fixed_byte = other.fixed_byte;
    offset = other.offset;
    partition_data_offset = other.partition_data_offset;
    blob = other.blob;
    return *this;
  }
  ContentSource& operator=(ContentSource&& other) noexcept
  {
    ContentBlob* const old = blob;
    type = other.type;
    fixed_byte = other.fixed_byte;
    offset = other.offset;
    partition_data_offset = other.partition_data_offset;
    blob = other.blob;
    other.blob = nullptr;
    if (old)
      ContentBlobRelease(old);
    return *this;
  }
  ~ContentSource()
  {
    if (blob)
      ContentBlobRelease(blob);
  }

  u8 type;
  u8 fixed_byte;
  u64 offset;
  u64 partition_data_offset;
  ContentBlob* blob;
};

ContentSource ContentFile(const char* path, u64 offset);
inline ContentSource ContentFile(const std::string& path, u64 offset)
{
  return ContentFile(path.c_str(), offset);
}
// Takes over the caller's reference.
ContentSource ContentMemory(ContentBlob* blob, u64 offset);
ContentSource ContentBytes(const void* data, size_t size);
ContentSource ContentPartition(u64 offset, u64 partition_data_offset);
ContentSource ContentFixedByte(u8 byte);

struct BuilderContentSource
{
  u64 m_offset = 0;
  u64 m_size = 0;
  ContentSource m_source;
};

struct FSTBuilderNode
{
  std::string m_filename;
  u64 m_size = 0;
  std::variant<std::vector<BuilderContentSource>, std::vector<FSTBuilderNode>> m_content;

  bool IsFile() const
  {
    return std::holds_alternative<std::vector<BuilderContentSource>>(m_content);
  }

  std::vector<BuilderContentSource>& GetFileContent()
  {
    return std::get<std::vector<BuilderContentSource>>(m_content);
  }

  const std::vector<BuilderContentSource>& GetFileContent() const
  {
    return std::get<std::vector<BuilderContentSource>>(m_content);
  }

  bool IsFolder() const { return std::holds_alternative<std::vector<FSTBuilderNode>>(m_content); }

  std::vector<FSTBuilderNode>& GetFolderContent()
  {
    return std::get<std::vector<FSTBuilderNode>>(m_content);
  }

  const std::vector<FSTBuilderNode>& GetFolderContent() const
  {
    return std::get<std::vector<FSTBuilderNode>>(m_content);
  }
};

// A content chunk placed at offset on the disc. Plain data: the container
// it sits in holds the blob's reference.
struct DiscContent
{
  u64 offset;
  u64 size;
  u64 src_offset;
  u64 partition_data_offset;
  ContentBlob* blob;
  u8 type;
  u8 fixed_byte;
};

// Content chunks sorted by end offset in one array, found by binary search.
class DiscContentContainer
{
public:
  DiscContentContainer();
  ~DiscContentContainer();
  DiscContentContainer(const DiscContentContainer& other);
  DiscContentContainer& operator=(const DiscContentContainer& other);
  DiscContentContainer(DiscContentContainer&& other) noexcept;
  DiscContentContainer& operator=(DiscContentContainer&& other) noexcept;

  void Add(u64 offset, u64 size, const ContentSource& source);
  void AddBytes(u64 offset, const void* data, size_t size);
  u64 CheckSizeAndAdd(u64 offset, const std::string& path);
  u64 CheckSizeAndAdd(u64 offset, u64 max_size, const std::string& path);

  bool Read(u64 offset, u64 length, u8* buffer, DirectoryBlobReader* blob) const;

private:
  void Clear();
  bool ReadContent(const DiscContent* content, u64* offset, u64* length, u8** buffer,
                   DirectoryBlobReader* blob) const;

  DiscContent* m_items;
  size_t m_count;
  size_t m_capacity;
};

class DirectoryBlobPartition
{
public:
  DirectoryBlobPartition() = default;
  DirectoryBlobPartition(std::string root_directory, std::optional<bool> is_wii);

  DirectoryBlobPartition(const DirectoryBlobPartition&) = default;
  DirectoryBlobPartition& operator=(const DirectoryBlobPartition&) = default;
  DirectoryBlobPartition(DirectoryBlobPartition&&) = default;
  DirectoryBlobPartition& operator=(DirectoryBlobPartition&&) = default;

  bool IsWii() const { return m_is_wii; }
  u64 GetDataSize() const { return m_data_size; }
  void SetDataSize(u64 size) { m_data_size = size; }
  const std::string& GetRootDirectory() const { return m_root_directory; }
  const DiscContentContainer& GetContents() const { return m_contents; }

  const std::array<u8, VolumeWii::AES_KEY_SIZE>& GetKey() const { return m_key; }
  void SetKey(std::array<u8, VolumeWii::AES_KEY_SIZE> key) { m_key = key; }

private:
  void SetDiscType(std::optional<bool> is_wii, const u8* disc_header);
  void SetBI2FromFile(const std::string& bi2_path);

  // Returns DOL address
  u64 SetApploaderFromFile(const std::string& path);
  // takes over the reference
  u64 SetApploader(ContentBlob* apploader, const std::string& log_path);
  // Returns FST address
  u64 SetDOLFromFile(const std::string& path, u64 dol_address, u8* disc_header);

  void BuildFSTFromFolder(const std::string& fst_root_path, u64 fst_address,
                          u8* disc_header);
  void BuildFST(std::vector<FSTBuilderNode> root_nodes, u64 fst_address,
                u8* disc_header);

  // FST creation
  void WriteEntryData(u8* fst_data, u32* entry_offset, u8 type, u32 name_offset,
                      u64 data_offset, u64 length, u32 address_shift);
  void WriteEntryName(u8* fst_data, u32* name_offset, const std::string& name,
                      u64 name_table_offset);
  void WriteDirectory(u8* fst_data, std::vector<FSTBuilderNode>* parent_entries,
                      u32* fst_offset, u32* name_offset, u64* data_offset, u32 parent_entry_index,
                      u64 name_table_offset);

  DiscContentContainer m_contents;

  std::array<u8, VolumeWii::AES_KEY_SIZE> m_key{};

  std::string m_root_directory;
  bool m_is_wii = false;
  bool m_is_triforce = false;
  // GameCube has no shift, Wii has 2 bit shift
  u32 m_address_shift = 0;

  u64 m_data_size = 0;
};

class DirectoryBlobReader final : public BlobReaderOfKind<BlobReaderKind::Directory>
{
  friend DiscContentContainer;

public:
  static DirectoryBlobReader* Create(const std::string& dol_path);

  DirectoryBlobReader(DirectoryBlobReader&&) = delete;
  DirectoryBlobReader& operator=(DirectoryBlobReader&&) = delete;
  ~DirectoryBlobReader();

  bool Read(u64 offset, u64 length, u8* buffer);
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* buffer, u64 partition_data_offset);

  BlobType GetBlobType() const;
  BlobReader* CopyReader() const;

  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const { return DataSizeType::Accurate; }

  u64 GetBlockSize() const { return 0; }
  bool HasFastRandomAccessInBlock() const { return true; }
  std::string GetCompressionMethod() const { return {}; }
  std::optional<int> GetCompressionLevel() const { return std::nullopt; }

  struct PartitionWithType
  {
    PartitionWithType(DirectoryBlobPartition&& partition_, PartitionType type_)
        : partition(std::move(partition_)), type(type_)
    {
    }

    DirectoryBlobPartition partition;
    PartitionType type;
  };

private:

  explicit DirectoryBlobReader(const std::string& game_partition_root,
                               const std::string& true_root);
  explicit DirectoryBlobReader(const DirectoryBlobReader& rhs);

  const DirectoryBlobPartition* GetPartition(u64 offset, u64 size, u64 partition_data_offset) const;

  bool EncryptPartitionData(u64 offset, u64 size, u8* buffer, u64 partition_data_offset,
                            u64 partition_data_decrypted_size);

  void SetNonpartitionDiscHeaderFromFile(const u8* partition_header,
                                         const std::string& game_partition_root);
  void SetNonpartitionDiscHeader(const u8* partition_header, const u8* header_bin,
                                 size_t header_bin_size);
  void SetWiiRegionDataFromFile(const std::string& game_partition_root);
  void SetWiiRegionData(const u8* wii_region_data, size_t size, const std::string& log_path);
  void SetPartitions(std::vector<PartitionWithType>&& partitions);
  void SetPartitionHeader(DirectoryBlobPartition* partition, u64 partition_address);

  // Reads from a host file, through a handle kept open across reads.
  bool ReadContentFile(const char* path, size_t path_len, u64 offset, u8* buffer, u64 size);

  // Recently read host files, kept open (and mapped where the platform maps)
  // so a read is a copy out of the page cache rather than an open, a map, an
  // unmap and a close. A reader owns a slot for the length of one read; a
  // read finding its slot taken opens the file for itself, as before.
  static constexpr u32 NUM_FILE_SLOTS = 16;
  struct FileSlot
  {
    retro_atomic_int_t busy;
    RFILE* file;
    const u8* map;
    u64 map_size;
    char path[PATH_MAX_LENGTH];
  };
  FileSlot m_file_slots[NUM_FILE_SLOTS];
  void InitFileSlots();

  // For GameCube:
  DirectoryBlobPartition m_gamecube_pseudopartition;

  // For Wii:
  DiscContentContainer m_nonpartition_contents;
  std::map<u64, DirectoryBlobPartition> m_partitions;
  WiiEncryptionCache m_encryption_cache;

  bool m_is_wii;
  bool m_encrypted;

  u64 m_data_size;
};

}  // namespace DiscIO
