// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/DirectoryBlob.h"

#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <streams/file_stream.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
// Reads as many bytes as the vector fits (or less, if the file is smaller).
// Returns the number of bytes read.
static size_t ReadFileToBuffer(const std::string& path, u8* buffer, size_t size);

static void PadToAddress(u64 start_address, u64* address, u64* length, u8** buffer);
static void Write32(u32 data, u32 offset, u8* buffer);

enum class PartitionType : u32
{
  Game = 0,
  Update = 1,
  Channel = 2,
  // There are more types used by Super Smash Bros. Brawl, but they don't have special names
};

// 0xFF is an arbitrarily picked value. Note that we can't use 0x00, because that means NTSC-J
constexpr u32 INVALID_REGION = 0xFF;

constexpr u32 PARTITION_DATA_OFFSET = 0x20000;

constexpr u8 ENTRY_SIZE = 0x0c;
constexpr u8 FILE_ENTRY = 0;
constexpr u8 DIRECTORY_ENTRY = 1;

ContentBlob* ContentBlobAlloc(size_t size)
{
  ContentBlob* blob = static_cast<ContentBlob*>(std::malloc(CONTENT_BLOB_HEADER + size));
  if (!blob)
    return nullptr;
  retro_atomic_int_init(&blob->refs, 1);
  blob->size = size;
  return blob;
}

ContentBlob* ContentBlobCreate(const void* data, size_t size)
{
  ContentBlob* blob = ContentBlobAlloc(size);
  if (blob && size)
    std::memcpy(ContentBlobData(blob), data, size);
  return blob;
}

// A zeroed blob of size octets.
static ContentBlob* ZeroedBlob(size_t size)
{
  ContentBlob* blob = ContentBlobAlloc(size);
  if (blob)
    std::memset(ContentBlobData(blob), 0, size);
  return blob;
}

void ContentBlobRetain(ContentBlob* blob)
{
  retro_atomic_inc_int(&blob->refs);
}

void ContentBlobRelease(ContentBlob* blob)
{
  if (retro_atomic_fetch_sub_int(&blob->refs, 1) == 1)
    std::free(blob);
}

ContentSource ContentFile(const char* path, u64 offset)
{
  ContentSource source;
  source.type = CONTENT_FILE;
  source.offset = offset;
  source.blob = ContentBlobCreate(path, std::strlen(path) + 1);
  return source;
}

ContentSource ContentMemory(ContentBlob* blob, u64 offset)
{
  ContentSource source;
  source.type = CONTENT_MEMORY;
  source.offset = offset;
  source.blob = blob;
  return source;
}

ContentSource ContentBytes(const void* data, size_t size)
{
  return ContentMemory(ContentBlobCreate(data, size), 0);
}

ContentSource ContentPartition(u64 offset, u64 partition_data_offset)
{
  ContentSource source;
  source.type = CONTENT_PARTITION;
  source.offset = offset;
  source.partition_data_offset = partition_data_offset;
  return source;
}

ContentSource ContentFixedByte(u8 byte)
{
  ContentSource source;
  source.type = CONTENT_FIXED_BYTE;
  source.fixed_byte = byte;
  return source;
}

DiscContentContainer::DiscContentContainer() : m_items(nullptr), m_count(0), m_capacity(0)
{
}

DiscContentContainer::~DiscContentContainer()
{
  Clear();
}

void DiscContentContainer::Clear()
{
  size_t i;
  for (i = 0; i < m_count; i++)
  {
    if (m_items[i].blob)
      ContentBlobRelease(m_items[i].blob);
  }
  std::free(m_items);
  m_items = nullptr;
  m_count = 0;
  m_capacity = 0;
}

DiscContentContainer::DiscContentContainer(const DiscContentContainer& other)
    : m_items(nullptr), m_count(0), m_capacity(0)
{
  *this = other;
}

DiscContentContainer& DiscContentContainer::operator=(const DiscContentContainer& other)
{
  size_t i;
  DiscContent* items;
  if (this == &other)
    return *this;
  items = other.m_count ?
              static_cast<DiscContent*>(std::malloc(other.m_count * sizeof(DiscContent))) :
              nullptr;
  if (other.m_count && !items)
    return *this;
  for (i = 0; i < other.m_count; i++)
  {
    items[i] = other.m_items[i];
    if (items[i].blob)
      ContentBlobRetain(items[i].blob);
  }
  Clear();
  m_items = items;
  m_count = other.m_count;
  m_capacity = other.m_count;
  return *this;
}

DiscContentContainer::DiscContentContainer(DiscContentContainer&& other) noexcept
    : m_items(other.m_items), m_count(other.m_count), m_capacity(other.m_capacity)
{
  other.m_items = nullptr;
  other.m_count = 0;
  other.m_capacity = 0;
}

DiscContentContainer& DiscContentContainer::operator=(DiscContentContainer&& other) noexcept
{
  if (this != &other)
  {
    Clear();
    m_items = other.m_items;
    m_count = other.m_count;
    m_capacity = other.m_capacity;
    other.m_items = nullptr;
    other.m_count = 0;
    other.m_capacity = 0;
  }
  return *this;
}

// the first chunk ending past offset, m_count if none does
static size_t FirstEndingAfter(const DiscContent* items, size_t count, u64 offset)
{
  size_t lo = 0, hi = count;
  while (lo < hi)
  {
    const size_t mid = lo + (hi - lo) / 2;
    if (items[mid].offset + items[mid].size > offset)
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo;
}

void DiscContentContainer::Add(u64 offset, u64 size, const ContentSource& source)
{
  size_t at;
  DiscContent* item;
  if (size == 0)
    return;

  // chunks are keyed by where they end; a second chunk ending at the same
  // place is not added
  at = FirstEndingAfter(m_items, m_count, offset + size - 1);
  if (at < m_count && m_items[at].offset + m_items[at].size == offset + size)
    return;

  if (m_count == m_capacity)
  {
    const size_t capacity = m_capacity ? m_capacity * 2 : 16;
    DiscContent* items =
        static_cast<DiscContent*>(std::realloc(m_items, capacity * sizeof(DiscContent)));
    if (!items)
      return;
    m_items = items;
    m_capacity = capacity;
  }
  std::memmove(m_items + at + 1, m_items + at, (m_count - at) * sizeof(DiscContent));
  m_count++;

  item = &m_items[at];
  item->offset = offset;
  item->size = size;
  item->src_offset = source.offset;
  item->partition_data_offset = source.partition_data_offset;
  item->blob = source.blob;
  item->type = source.type;
  item->fixed_byte = source.fixed_byte;
  if (item->blob)
    ContentBlobRetain(item->blob);
}

void DiscContentContainer::AddBytes(u64 offset, const void* data, size_t size)
{
  if (size)
    Add(offset, size, ContentBytes(data, size));
}

u64 DiscContentContainer::CheckSizeAndAdd(u64 offset, const std::string& path)
{
  const u64 size = File::GetSize(path);
  Add(offset, size, ContentFile(path, 0));
  return size;
}

u64 DiscContentContainer::CheckSizeAndAdd(u64 offset, u64 max_size, const std::string& path)
{
  const u64 file_size = File::GetSize(path);
  const u64 size = file_size < max_size ? file_size : max_size;
  Add(offset, size, ContentFile(path, 0));
  return size;
}

bool DiscContentContainer::ReadContent(const DiscContent* content, u64* offset, u64* length,
                                       u8** buffer, DirectoryBlobReader* blob) const
{
  const u64 offset_in_content = *offset - content->offset;
  u64 bytes_to_read;

  if (offset_in_content >= content->size)
    return true;
  bytes_to_read = content->size - offset_in_content;
  if (*length < bytes_to_read)
    bytes_to_read = *length;

  switch (content->type)
  {
  case CONTENT_FILE:
    if (!blob->ReadContentFile(reinterpret_cast<const char*>(ContentBlobData(content->blob)),
                               size_t(content->blob->size - 1),
                               content->src_offset + offset_in_content, *buffer, bytes_to_read))
    {
      return false;
    }
    break;
  case CONTENT_MEMORY:
    std::memcpy(*buffer, ContentBlobData(content->blob) + content->src_offset + offset_in_content,
                size_t(bytes_to_read));
    break;
  case CONTENT_PARTITION:
  {
    const u64 decrypted_size =
        content->size * VolumeWii::BLOCK_DATA_SIZE / VolumeWii::BLOCK_TOTAL_SIZE;
    if (!blob->EncryptPartitionData(content->src_offset + offset_in_content, bytes_to_read,
                                    *buffer, content->partition_data_offset, decrypted_size))
    {
      return false;
    }
    break;
  }
  case CONTENT_FIXED_BYTE:
    std::memset(*buffer, content->fixed_byte, size_t(bytes_to_read));
    break;
  default:
    PanicAlertFmt("DirectoryBlob: Invalid content source in DiscContent.");
    return false;
  }

  *length -= bytes_to_read;
  *buffer += bytes_to_read;
  *offset += bytes_to_read;
  return true;
}

bool DiscContentContainer::Read(u64 offset, u64 length, u8* buffer, DirectoryBlobReader* blob) const
{
  size_t i = FirstEndingAfter(m_items, m_count, offset);

  while (i < m_count && length > 0)
  {
    const DiscContent* content = &m_items[i];

    // Zero fill to start of DiscContent data
    PadToAddress(content->offset, &offset, &length, &buffer);

    if (length == 0)
      return true;

    if (!ReadContent(content, &offset, &length, &buffer, blob))
      return false;

    i++;
  }

  // Zero fill if we went beyond the last DiscContent
  std::memset(buffer, 0, static_cast<size_t>(length));

  return true;
}

static std::optional<PartitionType> ParsePartitionDirectoryName(const std::string& name)
{
  if (name.size() < 2)
    return {};

  if (!strcasecmp(name.c_str(), "DATA"))
    return PartitionType::Game;
  if (!strcasecmp(name.c_str(), "UPDATE"))
    return PartitionType::Update;
  if (!strcasecmp(name.c_str(), "CHANNEL"))
    return PartitionType::Channel;

  if (name[0] == 'P' || name[0] == 'p')
  {
    // e.g. "P-HA8E" (normally only used for Super Smash Bros. Brawl's VC partitions)
    if (name[1] == '-' && name.size() == 6)
    {
      const u32 result = Common::swap32(reinterpret_cast<const u8*>(name.data() + 2));
      return static_cast<PartitionType>(result);
    }

    // e.g. "P0"
    size_t digits = 1;
    while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9')
      digits++;
    if (digits == name.size())
    {
      u32 result;
      if (TryParse(name.substr(1), &result))
        return static_cast<PartitionType>(result);
    }
  }

  return {};
}

static bool IsDirectorySeparator(char c)
{
  return c == '/'
#ifdef _WIN32
         || c == '\\'
#endif
      ;
}

static bool PathCharactersEqual(char a, char b)
{
  return a == b || (IsDirectorySeparator(a) && IsDirectorySeparator(b));
}

static bool PathEndsWith(const std::string& path, const std::string_view suffix)
{
  if (suffix.size() > path.size())
    return false;

  std::string::const_iterator path_iterator = path.cend() - suffix.size();
  std::string_view::const_iterator suffix_iterator = suffix.cbegin();
  while (path_iterator != path.cend())
  {
    if (!PathCharactersEqual(*path_iterator, *suffix_iterator))
      return false;
    path_iterator++;
    suffix_iterator++;
  }

  return true;
}

static bool IsValidDirectoryBlob(const std::string& dol_path, std::string* partition_root,
                                 std::string* true_root = nullptr)
{
  if (!PathEndsWith(dol_path, "/sys/main.dol"))
    return false;

  static constexpr size_t chars_to_remove = std::string_view("sys/main.dol").size();
  *partition_root = dol_path.substr(0, dol_path.size() - chars_to_remove);

  if (File::GetSize(*partition_root + "sys/boot.bin") < 0x20)
    return false;

#ifdef _WIN32
  constexpr const char* dir_separator = "/\\";
#else
  constexpr char dir_separator = '/';
#endif
  if (true_root)
  {
    *true_root =
        dol_path.substr(0, dol_path.find_last_of(dir_separator, partition_root->size() - 2) + 1);
  }

  return true;
}

static bool ExistsAndIsValidDirectoryBlob(const std::string& dol_path)
{
  std::string partition_root;
  return File::Exists(dol_path) && IsValidDirectoryBlob(dol_path, &partition_root);
}

static bool IsInFilesDirectory(const std::string& path)
{
  size_t files_pos = std::string::npos;
  while (true)
  {
    files_pos = path.rfind("files", files_pos);
    if (files_pos == std::string::npos)
      return false;

    const size_t slash_before_pos = files_pos - 1;
    const size_t slash_after_pos = files_pos + 5;
    if ((files_pos == 0 || IsDirectorySeparator(path[slash_before_pos])) &&
        (slash_after_pos == path.size() || (IsDirectorySeparator(path[slash_after_pos]))) &&
        ExistsAndIsValidDirectoryBlob(path.substr(0, files_pos) + "sys/main.dol"))
    {
      return true;
    }

    --files_pos;
  }
}

static bool IsMainDolForNonGamePartition(const std::string& path)
{
  std::string partition_root, true_root;
  if (!IsValidDirectoryBlob(path, &partition_root, &true_root))
    return false;  // This is not a /sys/main.dol

  std::string partition_directory_name = partition_root.substr(true_root.size());
  partition_directory_name.pop_back();  // Remove trailing slash
  const std::optional<PartitionType> partition_type =
      ParsePartitionDirectoryName(partition_directory_name);
  if (!partition_type || *partition_type == PartitionType::Game)
    return false;  // volume_path is the game partition's /sys/main.dol

  const File::FSTEntry true_root_entry = File::ScanDirectoryTree(true_root, false);
  for (const File::FSTEntry& entry : true_root_entry.children)
  {
    if (entry.isDirectory &&
        ParsePartitionDirectoryName(entry.virtualName) == PartitionType::Game &&
        ExistsAndIsValidDirectoryBlob(entry.physicalName + "/sys/main.dol"))
    {
      return true;  // volume_path is the /sys/main.dol for a non-game partition
    }
  }

  return false;  // volume_path is the game partition's /sys/main.dol
}

static bool IsBootBin(const std::string& path)
{
  static constexpr std::string_view boot_bin = "/sys/boot.bin";
  if (!PathEndsWith(path, boot_bin))
    return false;

  const std::string partition_root = path.substr(0, path.size() - boot_bin.size());
  return File::Exists(partition_root + "/sys/main.dol");
}

static bool IsHeaderBin(const std::string& path)
{
  static constexpr std::string_view header_bin = "/disc/header.bin";
  if (!PathEndsWith(path, header_bin))
    return false;

  const std::string partition_root = path.substr(0, path.size() - header_bin.size());
  return File::Exists(partition_root + "/sys/main.dol");
}

bool ShouldHideFromGameList(const std::string& volume_path)
{
  return IsInFilesDirectory(volume_path) || IsMainDolForNonGamePartition(volume_path) ||
         IsBootBin(volume_path) || IsHeaderBin(volume_path);
}

std::unique_ptr<DirectoryBlobReader> DirectoryBlobReader::Create(const std::string& dol_path)
{
  std::string partition_root, true_root;
  if (!IsValidDirectoryBlob(dol_path, &partition_root, &true_root))
    return nullptr;

  return std::unique_ptr<DirectoryBlobReader>(new DirectoryBlobReader(partition_root, true_root));
}

DirectoryBlobReader::DirectoryBlobReader(const std::string& game_partition_root,
                                         const std::string& true_root)
    : m_encryption_cache(this)
{
  InitFileSlots();
  DirectoryBlobPartition game_partition(game_partition_root, {});
  m_is_wii = game_partition.IsWii();

  if (!m_is_wii)
  {
    m_gamecube_pseudopartition = std::move(game_partition);
    m_data_size = m_gamecube_pseudopartition.GetDataSize();
    m_encrypted = false;
  }
  else
  {
    u8 disc_header[DISCHEADER_SIZE];
    game_partition.GetContents().Read(DISCHEADER_ADDRESS, DISCHEADER_SIZE, disc_header, this);
    SetNonpartitionDiscHeaderFromFile(disc_header, game_partition_root);
    SetWiiRegionDataFromFile(game_partition_root);

    std::vector<PartitionWithType> partitions;
    partitions.emplace_back(std::move(game_partition), PartitionType::Game);

    std::string game_partition_directory_name = game_partition_root.substr(true_root.size());
    game_partition_directory_name.pop_back();  // Remove trailing slash
    if (ParsePartitionDirectoryName(game_partition_directory_name) == PartitionType::Game)
    {
      const File::FSTEntry true_root_entry = File::ScanDirectoryTree(true_root, false);
      for (const File::FSTEntry& entry : true_root_entry.children)
      {
        if (entry.isDirectory)
        {
          const std::optional<PartitionType> type = ParsePartitionDirectoryName(entry.virtualName);
          if (type && *type != PartitionType::Game)
          {
            partitions.emplace_back(DirectoryBlobPartition(entry.physicalName + "/", m_is_wii),
                                    *type);
          }
        }
      }
    }

    SetPartitions(std::move(partitions));
  }
}

DirectoryBlobReader::DirectoryBlobReader(const DirectoryBlobReader& rhs)
    : m_gamecube_pseudopartition(rhs.m_gamecube_pseudopartition),
      m_nonpartition_contents(rhs.m_nonpartition_contents), m_partitions(rhs.m_partitions),
      m_encryption_cache(this), m_is_wii(rhs.m_is_wii), m_encrypted(rhs.m_encrypted),
      m_data_size(rhs.m_data_size)
{
  InitFileSlots();
}

void DirectoryBlobReader::InitFileSlots()
{
  u32 i;
  for (i = 0; i < NUM_FILE_SLOTS; i++)
  {
    retro_atomic_int_init(&m_file_slots[i].busy, 0);
    m_file_slots[i].file = nullptr;
    m_file_slots[i].map = nullptr;
    m_file_slots[i].map_size = 0;
    m_file_slots[i].path[0] = '\0';
  }
}

DirectoryBlobReader::~DirectoryBlobReader()
{
  u32 i;
  for (i = 0; i < NUM_FILE_SLOTS; i++)
  {
    if (m_file_slots[i].file)
      filestream_close(m_file_slots[i].file);
  }
}

bool DirectoryBlobReader::ReadContentFile(const char* name, size_t name_len, u64 offset,
                                          u8* buffer, u64 size)
{
  FileSlot* slot;
  bool ok;
  u32 hash = 2166136261u;
  size_t i;

  for (i = 0; i < name_len; i++)
    hash = (hash ^ u8(name[i])) * 16777619u;
  slot = &m_file_slots[hash % NUM_FILE_SLOTS];

  if (name_len >= sizeof(slot->path) || retro_atomic_exchange_int(&slot->busy, 1))
  {
    File::DirectIOFile file(name, File::AccessMode::Read);
    return file.OffsetRead(offset, buffer, size);
  }

  if (!slot->file || std::strcmp(slot->path, name) != 0)
  {
    int64_t map_size = 0;
    if (slot->file)
      filestream_close(slot->file);
    slot->file = filestream_open(name, RETRO_VFS_FILE_ACCESS_READ,
                                 RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
    slot->map = slot->file ? filestream_get_mapped_ptr(slot->file, &map_size) : nullptr;
    slot->map_size = slot->map ? u64(map_size) : 0;
    std::memcpy(slot->path, name, name_len + 1);
  }

  if (slot->map)
  {
    ok = offset <= slot->map_size && size <= slot->map_size - offset;
    if (ok)
      std::memcpy(buffer, slot->map + offset, size_t(size));
  }
  else
  {
    ok = slot->file && filestream_seek(slot->file, s64(offset), RETRO_VFS_SEEK_POSITION_START) >= 0 &&
         filestream_read(slot->file, buffer, s64(size)) == s64(size);
  }

  retro_atomic_store_release_int(&slot->busy, 0);
  return ok;
}

bool DirectoryBlobReader::Read(u64 offset, u64 length, u8* buffer)
{
  if (offset + length > m_data_size)
    return false;

  return (m_is_wii ? m_nonpartition_contents : m_gamecube_pseudopartition.GetContents())
      .Read(offset, length, buffer, this);
}

const DirectoryBlobPartition* DirectoryBlobReader::GetPartition(u64 offset, u64 size,
                                                                u64 partition_data_offset) const
{
  const auto it = m_partitions.find(partition_data_offset);
  if (it == m_partitions.end())
    return nullptr;

  if (offset + size > it->second.GetDataSize())
    return nullptr;

  return &it->second;
}

bool DirectoryBlobReader::SupportsReadWiiDecrypted(u64 offset, u64 size,
                                                   u64 partition_data_offset) const
{
  return static_cast<bool>(GetPartition(offset, size, partition_data_offset));
}

bool DirectoryBlobReader::ReadWiiDecrypted(u64 offset, u64 size, u8* buffer,
                                           u64 partition_data_offset)
{
  const DirectoryBlobPartition* partition = GetPartition(offset, size, partition_data_offset);
  if (!partition)
    return false;

  return partition->GetContents().Read(offset, size, buffer, this);
}

bool DirectoryBlobReader::EncryptPartitionData(u64 offset, u64 size, u8* buffer,
                                               u64 partition_data_offset,
                                               u64 partition_data_decrypted_size)
{
  auto it = m_partitions.find(partition_data_offset);
  if (it == m_partitions.end())
    return false;

  if (!m_encrypted)
    return it->second.GetContents().Read(offset, size, buffer, this);

  return m_encryption_cache.EncryptGroups(offset, size, buffer, partition_data_offset,
                                          partition_data_decrypted_size, it->second.GetKey());
}

BlobType DirectoryBlobReader::GetBlobType() const
{
  return BlobType::DIRECTORY;
}

std::unique_ptr<BlobReader> DirectoryBlobReader::CopyReader() const
{
  return std::unique_ptr<DirectoryBlobReader>(new DirectoryBlobReader(*this));
}

u64 DirectoryBlobReader::GetRawSize() const
{
  // Not implemented
  return 0;
}

u64 DirectoryBlobReader::GetDataSize() const
{
  return m_data_size;
}

void DirectoryBlobReader::SetNonpartitionDiscHeaderFromFile(const u8* partition_header,
                                                            const std::string& game_partition_root)
{
  u8 header_bin[WII_NONPARTITION_DISCHEADER_SIZE];
  const size_t header_bin_size = ReadFileToBuffer(game_partition_root + "disc/header.bin",
                                                  header_bin, sizeof(header_bin));
  SetNonpartitionDiscHeader(partition_header, header_bin, header_bin_size);
}

void DirectoryBlobReader::SetNonpartitionDiscHeader(const u8* partition_header,
                                                    const u8* header_bin, size_t header_bin_size)
{
  ContentBlob* blob = ContentBlobAlloc(WII_NONPARTITION_DISCHEADER_SIZE);
  u8* header;
  if (!blob)
    return;
  header = ContentBlobData(blob);

  // If header.bin is missing or smaller than expected, use the content of sys/boot.bin instead
  std::memcpy(header, header_bin, header_bin_size);
  std::memcpy(header + header_bin_size, partition_header + header_bin_size,
              WII_NONPARTITION_DISCHEADER_SIZE - header_bin_size);

  // 0x60 and 0x61 are the only differences between the partition and non-partition headers
  if (header_bin_size < 0x60)
    header[0x60] = 0;
  if (header_bin_size < 0x61)
    header[0x61] = 0;

  m_encrypted = (header[0x60] | header[0x61] | header[0x62] | header[0x63]) == 0;

  m_nonpartition_contents.Add(WII_NONPARTITION_DISCHEADER_ADDRESS,
                              WII_NONPARTITION_DISCHEADER_SIZE, ContentMemory(blob, 0));
}

void DirectoryBlobReader::SetWiiRegionDataFromFile(const std::string& game_partition_root)
{
  u8 wii_region_data[WII_REGION_DATA_SIZE];
  const std::string region_bin_path = game_partition_root + "disc/region.bin";
  const size_t bytes_read =
      ReadFileToBuffer(region_bin_path, wii_region_data, sizeof(wii_region_data));
  SetWiiRegionData(wii_region_data, bytes_read, region_bin_path);
}

void DirectoryBlobReader::SetWiiRegionData(const u8* wii_region_data, size_t size,
                                           const std::string& log_path)
{
  ContentBlob* blob = ContentBlobAlloc(WII_REGION_DATA_SIZE);
  u8* region_data;
  if (!blob)
    return;
  region_data = ContentBlobData(blob);
  std::memset(region_data, 0, 0x10);
  std::memset(region_data + 0x10, 0x80, WII_REGION_DATA_SIZE - 0x10);
  Write32(INVALID_REGION, 0, region_data);

  std::memcpy(region_data, wii_region_data,
              size < WII_REGION_DATA_SIZE ? size_t(size) : size_t(WII_REGION_DATA_SIZE));

  if (size < 0x4)
    ERROR_LOG(DISCIO, "Couldn't read region from %s", log_path.c_str());
  else if (size < 0x20)
    ERROR_LOG(DISCIO, "Couldn't read age ratings from %s", log_path.c_str());

  m_nonpartition_contents.Add(WII_REGION_DATA_ADDRESS, WII_REGION_DATA_SIZE,
                              ContentMemory(blob, 0));
}

static bool PartitionLess(const DirectoryBlobReader::PartitionWithType& lhs,
                          const DirectoryBlobReader::PartitionWithType& rhs)
{
  if (lhs.type == rhs.type)
    return lhs.partition.GetRootDirectory() < rhs.partition.GetRootDirectory();

  // Ascending sort by partition type, except Update (1) comes before before Game (0)
  return (lhs.type > PartitionType::Update || rhs.type > PartitionType::Update) ?
             lhs.type < rhs.type :
             lhs.type > rhs.type;
}

void DirectoryBlobReader::SetPartitions(std::vector<PartitionWithType>&& partitions)
{
  // a handful of partitions: insertion sort
  for (size_t i = 1; i < partitions.size(); i++)
  {
    for (size_t j = i; j > 0 && PartitionLess(partitions[j], partitions[j - 1]); j--)
      std::swap(partitions[j], partitions[j - 1]);
  }

  u32 subtable_1_size = 0;
  while (subtable_1_size < partitions.size() && subtable_1_size < 3 &&
         partitions[subtable_1_size].type <= PartitionType::Channel)
  {
    ++subtable_1_size;
  }
  const u32 subtable_2_size = static_cast<u32>(partitions.size() - subtable_1_size);

  constexpr u32 PARTITION_TABLE_ADDRESS = 0x40000;
  constexpr u32 PARTITION_SUBTABLE1_OFFSET = 0x20;
  constexpr u32 PARTITION_SUBTABLE2_OFFSET = 0x40;
  const size_t partition_table_size = PARTITION_SUBTABLE2_OFFSET + subtable_2_size * 8;
  ContentBlob* const table_blob = ZeroedBlob(partition_table_size);
  if (!table_blob)
    return;
  u8* const partition_table = ContentBlobData(table_blob);

  Write32(subtable_1_size, 0x0, partition_table);
  Write32((PARTITION_TABLE_ADDRESS + PARTITION_SUBTABLE1_OFFSET) >> 2, 0x4, partition_table);
  if (subtable_2_size != 0)
  {
    Write32(subtable_2_size, 0x8, partition_table);
    Write32((PARTITION_TABLE_ADDRESS + PARTITION_SUBTABLE2_OFFSET) >> 2, 0xC, partition_table);
  }

  constexpr u64 STANDARD_UPDATE_PARTITION_ADDRESS = 0x50000;
  constexpr u64 STANDARD_GAME_PARTITION_ADDRESS = 0xF800000;
  u64 partition_address = STANDARD_UPDATE_PARTITION_ADDRESS;
  u64 offset_in_table = PARTITION_SUBTABLE1_OFFSET;
  for (size_t i = 0; i < partitions.size(); ++i)
  {
    if (i == subtable_1_size)
      offset_in_table = PARTITION_SUBTABLE2_OFFSET;

    if (partitions[i].type == PartitionType::Game)
    {
      if (partition_address < STANDARD_GAME_PARTITION_ADDRESS)
        partition_address = STANDARD_GAME_PARTITION_ADDRESS;
    }

    Write32(static_cast<u32>(partition_address >> 2), u32(offset_in_table), partition_table);
    offset_in_table += 4;
    Write32(static_cast<u32>(partitions[i].type), u32(offset_in_table), partition_table);
    offset_in_table += 4;

    SetPartitionHeader(&partitions[i].partition, partition_address);

    const u64 data_size =
        Common::AlignUp(partitions[i].partition.GetDataSize(), VolumeWii::BLOCK_DATA_SIZE);
    partitions[i].partition.SetDataSize(data_size);
    const u64 encrypted_data_size =
        (data_size / VolumeWii::BLOCK_DATA_SIZE) * VolumeWii::BLOCK_TOTAL_SIZE;
    const u64 partition_data_offset = partition_address + PARTITION_DATA_OFFSET;
    m_partitions.emplace(partition_data_offset, std::move(partitions[i].partition));
    m_nonpartition_contents.Add(partition_data_offset, encrypted_data_size,
                                ContentPartition(0, partition_data_offset));
    const u64 unaligned_next_partition_address = VolumeWii::OffsetInHashedPartitionToRawOffset(
        data_size, Partition(partition_address), PARTITION_DATA_OFFSET);
    partition_address = Common::AlignUp(unaligned_next_partition_address, 0x10000ull);
  }
  m_data_size = partition_address;

  m_nonpartition_contents.Add(PARTITION_TABLE_ADDRESS, partition_table_size,
                              ContentMemory(table_blob, 0));
}

// This function sets the header that's shortly before the start of the encrypted
// area, not the header that's right at the beginning of the encrypted area
void DirectoryBlobReader::SetPartitionHeader(DirectoryBlobPartition* partition,
                                             u64 partition_address)
{
  constexpr u32 TMD_OFFSET = 0x2c0;
  constexpr u32 H3_OFFSET = 0x4000;

  const std::string& partition_root = partition->GetRootDirectory();

  const u64 ticket_size = m_nonpartition_contents.CheckSizeAndAdd(
      partition_address + WII_PARTITION_TICKET_ADDRESS, WII_PARTITION_TICKET_SIZE,
      partition_root + "ticket.bin");

  const u64 tmd_size = m_nonpartition_contents.CheckSizeAndAdd(
      partition_address + TMD_OFFSET, IOS::ES::MAX_TMD_SIZE, partition_root + "tmd.bin");

  const u64 cert_offset = Common::AlignUp(TMD_OFFSET + tmd_size, 0x20ull);
  const u64 max_cert_size = H3_OFFSET - cert_offset;

  const u64 cert_size = m_nonpartition_contents.CheckSizeAndAdd(
      partition_address + cert_offset, max_cert_size, partition_root + "cert.bin");

  m_nonpartition_contents.CheckSizeAndAdd(partition_address + H3_OFFSET, WII_PARTITION_H3_SIZE,
                                          partition_root + "h3.bin");

  constexpr u32 PARTITION_HEADER_SIZE = 0x1c;
  const u64 data_size = Common::AlignUp(partition->GetDataSize(), 0x7c00) / 0x7c00 * 0x8000;
  u8 partition_header[PARTITION_HEADER_SIZE];
  Write32(static_cast<u32>(tmd_size), 0x0, partition_header);
  Write32(TMD_OFFSET >> 2, 0x4, partition_header);
  Write32(static_cast<u32>(cert_size), 0x8, partition_header);
  Write32(static_cast<u32>(cert_offset >> 2), 0x0C, partition_header);
  Write32(H3_OFFSET >> 2, 0x10, partition_header);
  Write32(PARTITION_DATA_OFFSET >> 2, 0x14, partition_header);
  Write32(static_cast<u32>(data_size >> 2), 0x18, partition_header);

  m_nonpartition_contents.AddBytes(partition_address + WII_PARTITION_TICKET_SIZE,
                                   partition_header, sizeof(partition_header));

  // read straight into the reader
  IOS::ES::TicketReader ticket;
  u8* const ticket_bytes = ticket.ResizeBytes(ticket_size);
  if (ticket_bytes)
  {
    m_nonpartition_contents.Read(partition_address + WII_PARTITION_TICKET_ADDRESS, ticket_size,
                                 ticket_bytes, this);
  }
  if (ticket.IsValid())
    partition->SetKey(ticket.GetTitleKey());
}

DirectoryBlobPartition::DirectoryBlobPartition(std::string root_directory,
                                               std::optional<bool> is_wii)
    : m_root_directory(std::move(root_directory))
{
  // the header is put together in place and added once the DOL and FST
  // addresses are in it
  ContentBlob* const header_blob = ZeroedBlob(DISCHEADER_SIZE);
  if (!header_blob)
    return;
  u8* const disc_header = ContentBlobData(header_blob);
  if (ReadFileToBuffer(m_root_directory + "sys/boot.bin", disc_header, DISCHEADER_SIZE) < 0x20)
    ERROR_LOG(DISCIO, "%s doesn't exist or is too small",
              (m_root_directory + "sys/boot.bin").c_str());

  SetDiscType(is_wii, disc_header);
  SetBI2FromFile(m_root_directory + "sys/bi2.bin");
  const u64 dol_address = SetApploaderFromFile(m_root_directory + "sys/apploader.img");
  const u64 fst_address =
      SetDOLFromFile(m_root_directory + "sys/main.dol", dol_address, disc_header);
  BuildFSTFromFolder(m_root_directory + "files/", fst_address, disc_header);

  m_contents.Add(DISCHEADER_ADDRESS, DISCHEADER_SIZE, ContentMemory(header_blob, 0));
}

void DirectoryBlobPartition::SetDiscType(std::optional<bool> is_wii, const u8* disc_header)
{
  if (is_wii.has_value())
  {
    m_is_wii = *is_wii;
  }
  else
  {
    m_is_wii = Common::swap32(&disc_header[0x18]) == WII_DISC_MAGIC;
    const bool is_gc = Common::swap32(&disc_header[0x1c]) == GAMECUBE_DISC_MAGIC;
    if (m_is_wii == is_gc)
    {
      ERROR_LOG(DISCIO, "Couldn't detect disc type based on disc header; assuming %s",
                m_is_wii ? "Wii" : "GameCube");
    }
  }

  m_address_shift = m_is_wii ? 2 : 0;
}

void DirectoryBlobPartition::SetBI2FromFile(const std::string& bi2_path)
{
  ContentBlob* const blob = ZeroedBlob(BI2_SIZE);
  size_t bytes_read;
  if (!blob)
    return;

  if (!m_is_wii)
    Write32(INVALID_REGION, 0x18, ContentBlobData(blob));

  bytes_read = ReadFileToBuffer(bi2_path, ContentBlobData(blob), BI2_SIZE);
  if (!m_is_wii && bytes_read < 0x1C)
    ERROR_LOG(DISCIO, "Couldn't read region from %s", bi2_path.c_str());

  m_contents.Add(BI2_ADDRESS, BI2_SIZE, ContentMemory(blob, 0));
}

u64 DirectoryBlobPartition::SetApploaderFromFile(const std::string& path)
{
  File::DirectIOFile file(path, File::AccessMode::Read);
  const size_t size = size_t(file.GetSize());
  ContentBlob* blob = ContentBlobAlloc(size);
  if (blob && size && !file.OffsetRead(0, ContentBlobData(blob), size))
    blob->size = 0;
  return SetApploader(blob, path);
}

u64 DirectoryBlobPartition::SetApploader(ContentBlob* apploader, const std::string& log_path)
{
  bool success = false;
  const u64 size = apploader ? apploader->size : 0;

  if (size < 0x20)
  {
    ERROR_LOG(DISCIO, "%s couldn't be accessed or is too small", log_path.c_str());
  }
  else
  {
    const u8* const data = ContentBlobData(apploader);
    const u64 apploader_size = 0x20 + Common::swap32(data + 0x14) + Common::swap32(data + 0x18);
    if (apploader_size != size)
      ERROR_LOG(DISCIO, "%s is the wrong size... Is it really an apploader?", log_path.c_str());
    else
      success = true;
  }

  if (!success)
  {
    // the header alone, made sure BS2 HLE doesn't try to run the apploader
    ContentBlob* const header = ZeroedBlob(0x20);
    if (apploader)
    {
      if (header)
        std::memcpy(ContentBlobData(header), ContentBlobData(apploader),
                    size < 0x20 ? size_t(size) : size_t(0x20));
      ContentBlobRelease(apploader);
    }
    apploader = header;
    if (!apploader)
      return Common::AlignUp(APPLOADER_ADDRESS + 0x20 + 0x20, 0x20ull);
    Write32(static_cast<u32>(-1), 0x10, ContentBlobData(apploader));
  }

  const u64 apploader_size = apploader->size;
  m_contents.Add(APPLOADER_ADDRESS, apploader_size, ContentMemory(apploader, 0));

  // Return DOL address, 32 byte aligned (plus 32 byte padding)
  return Common::AlignUp(APPLOADER_ADDRESS + apploader_size + 0x20, 0x20ull);
}

u64 DirectoryBlobPartition::SetDOLFromFile(const std::string& path, u64 dol_address,
                                           u8* disc_header)
{
  const u64 dol_size = m_contents.CheckSizeAndAdd(dol_address, path);

  Write32(static_cast<u32>(dol_address >> m_address_shift), 0x0420, disc_header);

  // Return FST address, 32 byte aligned (plus 32 byte padding)
  return Common::AlignUp(dol_address + dol_size + 0x20, 0x20ull);
}

static std::vector<FSTBuilderNode> ConvertFSTEntriesToBuilderNodes(const File::FSTEntry& parent)
{
  std::vector<FSTBuilderNode> nodes;
  nodes.reserve(parent.children.size());
  for (const File::FSTEntry& entry : parent.children)
  {
    std::variant<std::vector<BuilderContentSource>, std::vector<FSTBuilderNode>> content;
    if (entry.isDirectory)
    {
      content = ConvertFSTEntriesToBuilderNodes(entry);
    }
    else
    {
      content =
          std::vector<BuilderContentSource>{{0, entry.size, ContentFile(entry.physicalName, 0)}};
    }

    nodes.emplace_back(FSTBuilderNode{entry.virtualName, entry.size, std::move(content)});
  }
  return nodes;
}

void DirectoryBlobPartition::BuildFSTFromFolder(const std::string& fst_root_path, u64 fst_address,
                                                u8* disc_header)
{
  auto nodes = ConvertFSTEntriesToBuilderNodes(File::ScanDirectoryTree(fst_root_path, true));
  BuildFST(std::move(nodes), fst_address, disc_header);
}

static void ConvertUTF8NamesToSHIFTJIS(std::vector<FSTBuilderNode>* fst)
{
  for (FSTBuilderNode& entry : *fst)
  {
    if (entry.IsFolder())
      ConvertUTF8NamesToSHIFTJIS(&entry.GetFolderContent());
    entry.m_filename = UTF8ToSHIFTJIS(entry.m_filename);
  }
}

static u32 ComputeNameSize(const FSTBuilderNode* files, size_t count)
{
  u32 name_size = 0;
  for (size_t i = 0; i < count; i++)
  {
    const FSTBuilderNode& entry = files[i];
    if (entry.IsFolder())
    {
      const std::vector<FSTBuilderNode>& children = entry.GetFolderContent();
      name_size += ComputeNameSize(children.data(), children.size());
    }
    name_size += static_cast<u32>(entry.m_filename.length() + 1);
  }
  return name_size;
}

// Upper-cased ASCII order, then plain order, compared in place.
static bool NameLess(const FSTBuilderNode& one, const FSTBuilderNode& two)
{
  const unsigned char* a = reinterpret_cast<const unsigned char*>(one.m_filename.c_str());
  const unsigned char* b = reinterpret_cast<const unsigned char*>(two.m_filename.c_str());
  size_t i;
  for (i = 0;; i++)
  {
    unsigned ca = a[i], cb = b[i];
    if (ca >= 'a' && ca <= 'z')
      ca -= 'a' - 'A';
    if (cb >= 'a' && cb <= 'z')
      cb -= 'a' - 'A';
    if (ca != cb)
      return ca < cb;
    if (!a[i])
      break;
  }
  return one.m_filename < two.m_filename;
}

// In-place heap sort: no allocation, O(n log n); names are unique so
// stability does not matter.
static void SiftDown(FSTBuilderNode* nodes, size_t root, size_t count)
{
  for (;;)
  {
    size_t child = root * 2 + 1;
    if (child >= count)
      return;
    if (child + 1 < count && NameLess(nodes[child], nodes[child + 1]))
      child++;
    if (!NameLess(nodes[root], nodes[child]))
      return;
    std::swap(nodes[root], nodes[child]);
    root = child;
  }
}

static void SortByName(FSTBuilderNode* nodes, size_t count)
{
  size_t i;
  if (count < 2)
    return;
  for (i = count / 2; i > 0; i--)
    SiftDown(nodes, i - 1, count);
  for (i = count - 1; i > 0; i--)
  {
    std::swap(nodes[0], nodes[i]);
    SiftDown(nodes, 0, i);
  }
}

static size_t RecalculateFolderSizes(std::vector<FSTBuilderNode>* fst)
{
  size_t size = 0;
  for (FSTBuilderNode& entry : *fst)
  {
    ++size;
    if (entry.IsFile())
      continue;

    entry.m_size = RecalculateFolderSizes(&entry.GetFolderContent());
    size += entry.m_size;
  }
  return size;
}

void DirectoryBlobPartition::BuildFST(std::vector<FSTBuilderNode> root_nodes, u64 fst_address,
                                      u8* disc_header)
{
  ConvertUTF8NamesToSHIFTJIS(&root_nodes);

  u32 name_table_size = Common::AlignUp(ComputeNameSize(root_nodes.data(), root_nodes.size()), 1ull << m_address_shift);

  // 1 extra for the root entry
  u64 total_entries = RecalculateFolderSizes(&root_nodes) + 1;

  const u64 name_table_offset = total_entries * ENTRY_SIZE;
  // built in place, then added as it stands
  const u64 fst_size = name_table_offset + name_table_size;
  ContentBlob* const fst_blob = ZeroedBlob(size_t(fst_size));
  if (!fst_blob)
    return;
  u8* const fst_data = ContentBlobData(fst_blob);

  // 32 KiB aligned start of data on disc
  u64 current_data_address = Common::AlignUp(fst_address + fst_size, 0x8000ull);

  u32 fst_offset = 0;   // Offset within FST data
  u32 name_offset = 0;  // Offset within name table
  u32 root_offset = 0;  // Offset of root of FST

  // write root entry
  WriteEntryData(fst_data, &fst_offset, DIRECTORY_ENTRY, 0, 0, total_entries, m_address_shift);

  WriteDirectory(fst_data, &root_nodes, &fst_offset, &name_offset, &current_data_address,
                 root_offset, name_table_offset);

  // overflow check, compare the aligned name offset with the aligned name table size
  ASSERT(Common::AlignUp(name_offset, 1ull << m_address_shift) == name_table_size);

  // write FST size and location
  Write32((u32)(fst_address >> m_address_shift), 0x0424, disc_header);
  Write32((u32)(fst_size >> m_address_shift), 0x0428, disc_header);
  Write32((u32)(fst_size >> m_address_shift), 0x042c, disc_header);

  m_contents.Add(fst_address, fst_size, ContentMemory(fst_blob, 0));

  m_data_size = current_data_address;
}

void DirectoryBlobPartition::WriteEntryData(u8* fst_data, u32* entry_offset, u8 type,
                                            u32 name_offset, u64 data_offset, u64 length,
                                            u32 address_shift)
{
  fst_data[(*entry_offset)++] = type;

  fst_data[(*entry_offset)++] = (name_offset >> 16) & 0xff;
  fst_data[(*entry_offset)++] = (name_offset >> 8) & 0xff;
  fst_data[(*entry_offset)++] = (name_offset) & 0xff;

  Write32((u32)(data_offset >> address_shift), *entry_offset, fst_data);
  *entry_offset += 4;

  Write32((u32)length, *entry_offset, fst_data);
  *entry_offset += 4;
}

void DirectoryBlobPartition::WriteEntryName(u8* fst_data, u32* name_offset,
                                            const std::string& name, u64 name_table_offset)
{
  std::memcpy(fst_data + *name_offset + name_table_offset, name.c_str(), name.length() + 1);

  *name_offset += (u32)(name.length() + 1);
}

void DirectoryBlobPartition::WriteDirectory(u8* fst_data,
                                            std::vector<FSTBuilderNode>* parent_entries,
                                            u32* fst_offset, u32* name_offset, u64* data_offset,
                                            u32 parent_entry_index, u64 name_table_offset)
{
  std::vector<FSTBuilderNode>& sorted_entries = *parent_entries;

  // Sort for determinism
  SortByName(sorted_entries.data(), sorted_entries.size());

  for (FSTBuilderNode& entry : sorted_entries)
  {
    if (entry.IsFolder())
    {
      u32 entry_index = *fst_offset / ENTRY_SIZE;
      WriteEntryData(fst_data, fst_offset, DIRECTORY_ENTRY, *name_offset, parent_entry_index,
                     entry_index + entry.m_size + 1, 0);
      WriteEntryName(fst_data, name_offset, entry.m_filename, name_table_offset);

      auto& child_nodes = entry.GetFolderContent();
      WriteDirectory(fst_data, &child_nodes, fst_offset, name_offset, data_offset, entry_index,
                     name_table_offset);
    }
    else
    {
      // Skip Triforce DIMM so the file doesn't overlap with it. Any portion of
      // a file placed in 0x1F000000-0x1F800000 would be shadowed by SRAM on the
      // AMMediaboard and read back as DIMM contents instead of disc data.
      if (m_is_triforce && *data_offset < 0x1F800000 && *data_offset + entry.m_size > 0x1F000000)
      {
        *data_offset = 0x1F800000ull;
      }

      // put entry in FST
      WriteEntryData(fst_data, fst_offset, FILE_ENTRY, *name_offset, *data_offset, entry.m_size,
                     m_address_shift);
      WriteEntryName(fst_data, name_offset, entry.m_filename, name_table_offset);

      // write entry to virtual disc
      auto& contents = entry.GetFileContent();
      for (BuilderContentSource& content : contents)
      {
        m_contents.Add(*data_offset + content.m_offset, content.m_size,
                       content.m_source);
      }

      // For most files, an alignment of 0x20 is enough. But some files need 0x8000,
      // in particular all DTK audio files in GameCube games.
      // TODO: Do any Wii games need 0x8000 alignment?
      const u64 data_alignment = m_is_triforce ? 0x20ull : 0x8000ull;
      *data_offset = Common::AlignUp(*data_offset + entry.m_size, data_alignment);
    }
  }
}

// Up to size octets of the file at path into buffer; returns how many.
static size_t ReadFileToBuffer(const std::string& path, u8* buffer, size_t size)
{
  File::DirectIOFile file(path, File::AccessMode::Read);
  const u64 file_size = file.GetSize();
  const size_t n = file_size < size ? size_t(file_size) : size;
  return n && file.OffsetRead(0, buffer, n) ? n : 0;
}

static void PadToAddress(u64 start_address, u64* address, u64* length, u8** buffer)
{
  if (start_address > *address && *length > 0)
  {
    u64 padBytes = start_address - *address;
    if (*length < padBytes)
      padBytes = *length;
    memset(*buffer, 0, (size_t)padBytes);
    *length -= padBytes;
    *buffer += padBytes;
    *address += padBytes;
  }
}

static void Write32(u32 data, u32 offset, u8* buffer)
{
  buffer[offset] = u8(data >> 24);
  buffer[offset + 1] = u8(data >> 16);
  buffer[offset + 2] = u8(data >> 8);
  buffer[offset + 3] = u8(data);
}

}  // namespace DiscIO
