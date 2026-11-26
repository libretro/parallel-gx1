// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/DirectoryBlob.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <encodings/shiftjis.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonPaths.h"
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
// Reads as many bytes as the buffer fits (or less, if the file is smaller).
// Returns the number of bytes read.
static size_t ReadFileToBuffer(const char* path, u8* buffer, size_t size);

// The longest name, its NUL included, that RootPath puts after a root
static constexpr size_t ROOT_NAME_SIZE = 32;

// A heap buffer holding root, with room for a name after it; nullptr when out of memory
static char* RootPathBuffer(const char* root, size_t* root_length)
{
  *root_length = std::strlen(root);
  char* const buffer = static_cast<char*>(std::malloc(*root_length + ROOT_NAME_SIZE));
  if (buffer)
    std::memcpy(buffer, root, *root_length);
  return buffer;
}

// The root in buffer followed by name
static const char* RootPath(char* buffer, size_t root_length, const char* name)
{
  const size_t size = std::strlen(name) + 1;
  ASSERT(size <= ROOT_NAME_SIZE);
  std::memcpy(buffer + root_length, name, size);
  return buffer;
}

// A copy of path on the heap, freed with free(); nullptr when out of memory
static char* CopyPath(const char* path)
{
  const size_t size = std::strlen(path) + 1;
  char* const copy = static_cast<char*>(std::malloc(size));
  if (copy)
    std::memcpy(copy, path, size);
  return copy;
}

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

u64 DiscContentContainer::CheckSizeAndAdd(u64 offset, const char* path)
{
  const u64 size = File::GetSize(path);
  Add(offset, size, ContentFile(path, 0));
  return size;
}

u64 DiscContentContainer::CheckSizeAndAdd(u64 offset, u64 max_size, const char* path)
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
    PanicAlert("DirectoryBlob: Invalid content source in DiscContent.");
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

static bool ParsePartitionDirectoryName(const char* name, PartitionType* type)
{
  const size_t length = std::strlen(name);
  if (length < 2)
    return false;

  if (!strcasecmp(name, "DATA"))
  {
    *type = PartitionType::Game;
    return true;
  }
  if (!strcasecmp(name, "UPDATE"))
  {
    *type = PartitionType::Update;
    return true;
  }
  if (!strcasecmp(name, "CHANNEL"))
  {
    *type = PartitionType::Channel;
    return true;
  }

  if (name[0] == 'P' || name[0] == 'p')
  {
    // e.g. "P-HA8E" (normally only used for Super Smash Bros. Brawl's VC partitions)
    if (name[1] == '-' && length == 6)
    {
      const u32 result = Common::swap32(reinterpret_cast<const u8*>(name + 2));
      *type = static_cast<PartitionType>(result);
      return true;
    }

    // e.g. "P0"
    size_t digits = 1;
    while (digits < length && name[digits] >= '0' && name[digits] <= '9')
      digits++;
    if (digits == length)
    {
      u32 result;
      if (TryParse(name + 1, &result))
      {
        *type = static_cast<PartitionType>(result);
        return true;
      }
    }
  }

  return false;
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

static bool PathEndsWith(const char* path, size_t length, const char* suffix)
{
  const size_t suffix_length = std::strlen(suffix);
  if (suffix_length > length)
    return false;

  path += length - suffix_length;
  for (size_t i = 0; i < suffix_length; i++)
  {
    if (!PathCharactersEqual(path[i], suffix[i]))
      return false;
  }

  return true;
}

// The game partition's root, a heap copy of dol_path up to its "sys/main.dol", and the length of
// the true root, the directory holding it; nullptr when dol_path is not a directory blob's
static char* IsValidDirectoryBlob(const char* dol_path, size_t* true_root_length)
{
  static const char main_dol[] = "sys/main.dol";
  const size_t dol_length = std::strlen(dol_path);
  if (!PathEndsWith(dol_path, dol_length, "/sys/main.dol"))
    return nullptr;

  const size_t root_length = dol_length - (sizeof(main_dol) - 1);
  char* const partition_root = static_cast<char*>(std::malloc(root_length + ROOT_NAME_SIZE));
  if (!partition_root)
    return nullptr;
  std::memcpy(partition_root, dol_path, root_length);

  if (File::GetSize(RootPath(partition_root, root_length, "sys/boot.bin")) < 0x20)
  {
    std::free(partition_root);
    return nullptr;
  }
  partition_root[root_length] = '\0';

  // the last separator before the partition root's own trailing one, searching the whole path
  // when the root is that separator alone
  size_t end = root_length >= 2 ? root_length - 1 : dol_length;
  while (end > 0 && !IsDirectorySeparator(dol_path[end - 1]))
    end--;
  *true_root_length = end;

  return partition_root;
}

// The partitions found, with their types; the arrays grow by doubling
struct PartitionList
{
  DirectoryBlobPartition* partitions;  // new[]
  PartitionType* types;                // malloc
  u32 count;
  u32 capacity;
};

static void AddPartition(PartitionList* list, DirectoryBlobPartition&& partition,
                         PartitionType type)
{
  if (list->count == list->capacity)
  {
    const u32 capacity = list->capacity ? list->capacity * 2 : 4;
    PartitionType* const types =
        static_cast<PartitionType*>(std::realloc(list->types, capacity * sizeof(PartitionType)));
    if (!types)
      return;
    list->types = types;
    DirectoryBlobPartition* const partitions = new DirectoryBlobPartition[capacity];
    for (u32 i = 0; i < list->count; i++)
      partitions[i] = std::move(list->partitions[i]);
    delete[] list->partitions;
    list->partitions = partitions;
    list->capacity = capacity;
  }
  list->partitions[list->count] = std::move(partition);
  list->types[list->count] = type;
  list->count++;
}

DirectoryBlobReader* DirectoryBlobReader::Create(const char* dol_path)
{
  size_t true_root_length;
  char* const partition_root = IsValidDirectoryBlob(dol_path, &true_root_length);
  if (!partition_root)
    return nullptr;

  DirectoryBlobReader* const reader = new DirectoryBlobReader(partition_root, true_root_length);
  std::free(partition_root);
  return reader;
}

DirectoryBlobReader::DirectoryBlobReader(const char* game_partition_root, size_t true_root_length)
    : m_partitions(nullptr), m_partition_data_offsets(nullptr), m_partition_count(0),
      m_encryption_cache(this)
{
  InitFileSlots();
  DirectoryBlobPartition game_partition(game_partition_root, nullptr);
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

    PartitionList partitions = {nullptr, nullptr, 0, 0};
    AddPartition(&partitions, std::move(game_partition), PartitionType::Game);

    // the game partition's directory name, without its trailing slash
    const size_t root_length = std::strlen(game_partition_root);
    const size_t name_length =
        true_root_length < root_length ? root_length - true_root_length - 1 : 0;
    char* const game_partition_directory_name = static_cast<char*>(std::malloc(name_length + 1));
    PartitionType root_type;
    if (game_partition_directory_name)
    {
      std::memcpy(game_partition_directory_name, game_partition_root + true_root_length,
                  name_length);
      game_partition_directory_name[name_length] = '\0';
    }
    if (game_partition_directory_name &&
        ParsePartitionDirectoryName(game_partition_directory_name, &root_type) &&
        root_type == PartitionType::Game)
    {
      // the true root's other partition directories: true root, '/', name, '/'
      size_t base_length = true_root_length;
      while (base_length > 1 && game_partition_root[base_length - 1] == DIR_SEP_CHR)
        base_length--;
      const bool add_separator = base_length && game_partition_root[base_length - 1] != DIR_SEP_CHR;
      char* const path = static_cast<char*>(std::malloc(base_length + 1 + PATH_MAX_LENGTH));
      struct RDIR* dir = nullptr;
      if (path)
      {
        std::memcpy(path, game_partition_root, base_length);
        path[base_length] = '\0';
        dir = retro_opendir_include_hidden(path, true);
        if (add_separator)
          path[base_length++] = DIR_SEP_CHR;
      }
      while (dir && retro_readdir(dir))
      {
        const char* const name = retro_dirent_get_name(dir);
        size_t length;
        PartitionType type;
        if (!name || !std::strcmp(name, ".") || !std::strcmp(name, "..") ||
            !retro_dirent_is_dir(dir, nullptr) || !ParsePartitionDirectoryName(name, &type) ||
            type == PartitionType::Game)
        {
          continue;
        }
        length = std::strlen(name);
        if (length + 2 > PATH_MAX_LENGTH)
          continue;
        std::memcpy(path + base_length, name, length);
        path[base_length + length] = DIR_SEP_CHR;
        path[base_length + length + 1] = '\0';
        AddPartition(&partitions, DirectoryBlobPartition(path, &m_is_wii), type);
      }
      if (dir)
        retro_closedir(dir);
      std::free(path);
    }
    std::free(game_partition_directory_name);

    SetPartitions(partitions.partitions, partitions.types, partitions.count);
  }
}

DirectoryBlobReader::DirectoryBlobReader(const DirectoryBlobReader& rhs)
    : m_gamecube_pseudopartition(rhs.m_gamecube_pseudopartition),
      m_nonpartition_contents(rhs.m_nonpartition_contents), m_partitions(nullptr),
      m_partition_data_offsets(nullptr), m_partition_count(0), m_encryption_cache(this),
      m_is_wii(rhs.m_is_wii), m_encrypted(rhs.m_encrypted), m_data_size(rhs.m_data_size)
{
  InitFileSlots();
  if (rhs.m_partition_count)
  {
    m_partitions = new DirectoryBlobPartition[rhs.m_partition_count];
    m_partition_data_offsets = new u64[rhs.m_partition_count];
    for (u32 i = 0; i < rhs.m_partition_count; i++)
    {
      m_partitions[i] = rhs.m_partitions[i];
      m_partition_data_offsets[i] = rhs.m_partition_data_offsets[i];
    }
    m_partition_count = rhs.m_partition_count;
  }
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
  delete[] m_partitions;
  delete[] m_partition_data_offsets;
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
  const DirectoryBlobPartition* const partition = FindPartition(partition_data_offset);
  if (!partition || offset + size > partition->GetDataSize())
    return nullptr;
  return partition;
}

const DirectoryBlobPartition* DirectoryBlobReader::FindPartition(u64 partition_data_offset) const
{
  for (u32 i = 0; i < m_partition_count; i++)
  {
    if (m_partition_data_offsets[i] == partition_data_offset)
      return &m_partitions[i];
  }
  return nullptr;
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
  const DirectoryBlobPartition* const partition = FindPartition(partition_data_offset);
  if (!partition)
    return false;

  if (!m_encrypted)
    return partition->GetContents().Read(offset, size, buffer, this);

  return m_encryption_cache.EncryptGroups(offset, size, buffer, partition_data_offset,
                                          partition_data_decrypted_size, partition->GetKey());
}

BlobType DirectoryBlobReader::GetBlobType() const
{
  return BlobType::DIRECTORY;
}

BlobReader* DirectoryBlobReader::CopyReader() const
{
  return new DirectoryBlobReader(*this);
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
                                                            const char* game_partition_root)
{
  u8 header_bin[WII_NONPARTITION_DISCHEADER_SIZE];
  size_t root_length;
  char* const path = RootPathBuffer(game_partition_root, &root_length);
  const size_t header_bin_size =
      path ? ReadFileToBuffer(RootPath(path, root_length, "disc/header.bin"), header_bin,
                              sizeof(header_bin)) :
             0;
  std::free(path);
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

void DirectoryBlobReader::SetWiiRegionDataFromFile(const char* game_partition_root)
{
  u8 wii_region_data[WII_REGION_DATA_SIZE];
  size_t root_length;
  char* const path = RootPathBuffer(game_partition_root, &root_length);
  if (!path)
    return;
  const char* const region_bin_path = RootPath(path, root_length, "disc/region.bin");
  const size_t bytes_read =
      ReadFileToBuffer(region_bin_path, wii_region_data, sizeof(wii_region_data));
  SetWiiRegionData(wii_region_data, bytes_read, region_bin_path);
  std::free(path);
}

void DirectoryBlobReader::SetWiiRegionData(const u8* wii_region_data, size_t size,
                                           const char* log_path)
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
    ERROR_LOG(DISCIO, "Couldn't read region from %s", log_path);
  else if (size < 0x20)
    ERROR_LOG(DISCIO, "Couldn't read age ratings from %s", log_path);

  m_nonpartition_contents.Add(WII_REGION_DATA_ADDRESS, WII_REGION_DATA_SIZE,
                              ContentMemory(blob, 0));
}

static bool PartitionLess(const DirectoryBlobPartition* partitions, const PartitionType* types,
                          u32 lhs, u32 rhs)
{
  if (types[lhs] == types[rhs])
  {
    return std::strcmp(partitions[lhs].GetRootDirectory(), partitions[rhs].GetRootDirectory()) < 0;
  }

  // Ascending sort by partition type, except Update (1) comes before before Game (0)
  return (types[lhs] > PartitionType::Update || types[rhs] > PartitionType::Update) ?
             types[lhs] < types[rhs] :
             types[lhs] > types[rhs];
}

void DirectoryBlobReader::SetPartitions(DirectoryBlobPartition* partitions, PartitionType* types,
                                        u32 count)
{
  // a handful of partitions: insertion sort of their indices
  u32* const order = static_cast<u32*>(std::malloc((count ? count : 1) * sizeof(u32)));
  if (!order)
  {
    delete[] partitions;
    std::free(types);
    return;
  }
  for (u32 i = 0; i < count; i++)
  {
    u32 j;
    order[i] = i;
    for (j = i; j > 0 && PartitionLess(partitions, types, order[j], order[j - 1]); j--)
    {
      const u32 moved = order[j];
      order[j] = order[j - 1];
      order[j - 1] = moved;
    }
  }

  u32 subtable_1_size = 0;
  while (subtable_1_size < count && subtable_1_size < 3 &&
         types[order[subtable_1_size]] <= PartitionType::Channel)
  {
    ++subtable_1_size;
  }
  const u32 subtable_2_size = count - subtable_1_size;

  constexpr u32 PARTITION_TABLE_ADDRESS = 0x40000;
  constexpr u32 PARTITION_SUBTABLE1_OFFSET = 0x20;
  constexpr u32 PARTITION_SUBTABLE2_OFFSET = 0x40;
  const size_t partition_table_size = PARTITION_SUBTABLE2_OFFSET + subtable_2_size * 8;
  ContentBlob* const table_blob = ZeroedBlob(partition_table_size);
  if (!table_blob)
  {
    std::free(order);
    delete[] partitions;
    std::free(types);
    return;
  }
  u8* const partition_table = ContentBlobData(table_blob);

  Write32(subtable_1_size, 0x0, partition_table);
  Write32((PARTITION_TABLE_ADDRESS + PARTITION_SUBTABLE1_OFFSET) >> 2, 0x4, partition_table);
  if (subtable_2_size != 0)
  {
    Write32(subtable_2_size, 0x8, partition_table);
    Write32((PARTITION_TABLE_ADDRESS + PARTITION_SUBTABLE2_OFFSET) >> 2, 0xC, partition_table);
  }

  m_partitions = new DirectoryBlobPartition[count];
  m_partition_data_offsets = new u64[count];

  constexpr u64 STANDARD_UPDATE_PARTITION_ADDRESS = 0x50000;
  constexpr u64 STANDARD_GAME_PARTITION_ADDRESS = 0xF800000;
  u64 partition_address = STANDARD_UPDATE_PARTITION_ADDRESS;
  u64 offset_in_table = PARTITION_SUBTABLE1_OFFSET;
  for (u32 i = 0; i < count; ++i)
  {
    DirectoryBlobPartition& partition = partitions[order[i]];
    const PartitionType type = types[order[i]];
    if (i == subtable_1_size)
      offset_in_table = PARTITION_SUBTABLE2_OFFSET;

    if (type == PartitionType::Game)
    {
      if (partition_address < STANDARD_GAME_PARTITION_ADDRESS)
        partition_address = STANDARD_GAME_PARTITION_ADDRESS;
    }

    Write32(static_cast<u32>(partition_address >> 2), u32(offset_in_table), partition_table);
    offset_in_table += 4;
    Write32(static_cast<u32>(type), u32(offset_in_table), partition_table);
    offset_in_table += 4;

    SetPartitionHeader(&partition, partition_address);

    const u64 data_size = Common::AlignUp(partition.GetDataSize(), VolumeWii::BLOCK_DATA_SIZE);
    partition.SetDataSize(data_size);
    const u64 encrypted_data_size =
        (data_size / VolumeWii::BLOCK_DATA_SIZE) * VolumeWii::BLOCK_TOTAL_SIZE;
    const u64 partition_data_offset = partition_address + PARTITION_DATA_OFFSET;
    m_partitions[m_partition_count] = std::move(partition);
    m_partition_data_offsets[m_partition_count] = partition_data_offset;
    m_partition_count++;
    m_nonpartition_contents.Add(partition_data_offset, encrypted_data_size,
                                ContentPartition(0, partition_data_offset));
    const u64 unaligned_next_partition_address = VolumeWii::OffsetInHashedPartitionToRawOffset(
        data_size, Partition(partition_address), PARTITION_DATA_OFFSET);
    partition_address = Common::AlignUp(unaligned_next_partition_address, 0x10000ull);
  }
  m_data_size = partition_address;
  std::free(order);
  delete[] partitions;
  std::free(types);

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

  size_t root_length;
  char* const path = RootPathBuffer(partition->GetRootDirectory(), &root_length);
  if (!path)
    return;

  const u64 ticket_size = m_nonpartition_contents.CheckSizeAndAdd(
      partition_address + WII_PARTITION_TICKET_ADDRESS, WII_PARTITION_TICKET_SIZE,
      RootPath(path, root_length, "ticket.bin"));

  const u64 tmd_size =
      m_nonpartition_contents.CheckSizeAndAdd(partition_address + TMD_OFFSET, IOS::ES::MAX_TMD_SIZE,
                                              RootPath(path, root_length, "tmd.bin"));

  const u64 cert_offset = Common::AlignUp(TMD_OFFSET + tmd_size, 0x20ull);
  const u64 max_cert_size = H3_OFFSET - cert_offset;

  const u64 cert_size = m_nonpartition_contents.CheckSizeAndAdd(
      partition_address + cert_offset, max_cert_size, RootPath(path, root_length, "cert.bin"));

  m_nonpartition_contents.CheckSizeAndAdd(partition_address + H3_OFFSET, WII_PARTITION_H3_SIZE,
                                          RootPath(path, root_length, "h3.bin"));
  std::free(path);

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
  {
    u8 title_key[16];
    ticket.GetTitleKey(title_key);
    partition->SetKey(title_key);
  }
}

DirectoryBlobPartition::DirectoryBlobPartition(const char* root_directory, const bool* is_wii)
    : m_root_directory(CopyPath(root_directory))
{
  // the header is put together in place and added once the DOL and FST
  // addresses are in it
  size_t root_length;
  char* const path = RootPathBuffer(root_directory, &root_length);
  ContentBlob* const header_blob = path ? ZeroedBlob(DISCHEADER_SIZE) : nullptr;
  if (!header_blob)
  {
    std::free(path);
    return;
  }
  u8* const disc_header = ContentBlobData(header_blob);
  if (ReadFileToBuffer(RootPath(path, root_length, "sys/boot.bin"), disc_header, DISCHEADER_SIZE) <
      0x20)
  {
    ERROR_LOG(DISCIO, "%s doesn't exist or is too small", path);
  }

  SetDiscType(is_wii, disc_header);
  SetBI2FromFile(RootPath(path, root_length, "sys/bi2.bin"));
  const u64 dol_address = SetApploaderFromFile(RootPath(path, root_length, "sys/apploader.img"));
  const u64 fst_address =
      SetDOLFromFile(RootPath(path, root_length, "sys/main.dol"), dol_address, disc_header);
  BuildFSTFromFolder(RootPath(path, root_length, "files/"), fst_address, disc_header);
  std::free(path);

  m_contents.Add(DISCHEADER_ADDRESS, DISCHEADER_SIZE, ContentMemory(header_blob, 0));
}

DirectoryBlobPartition::DirectoryBlobPartition(const DirectoryBlobPartition& other)
    : m_contents(other.m_contents),
      m_root_directory(other.m_root_directory ? CopyPath(other.m_root_directory) : nullptr),
      m_is_wii(other.m_is_wii), m_is_triforce(other.m_is_triforce),
      m_address_shift(other.m_address_shift), m_data_size(other.m_data_size)
{
  std::memcpy(m_key, other.m_key, sizeof(m_key));
}

DirectoryBlobPartition& DirectoryBlobPartition::operator=(const DirectoryBlobPartition& other)
{
  if (this != &other)
  {
    char* const root = other.m_root_directory ? CopyPath(other.m_root_directory) : nullptr;
    std::free(m_root_directory);
    m_root_directory = root;
    m_contents = other.m_contents;
    std::memcpy(m_key, other.m_key, sizeof(m_key));
    m_is_wii = other.m_is_wii;
    m_is_triforce = other.m_is_triforce;
    m_address_shift = other.m_address_shift;
    m_data_size = other.m_data_size;
  }
  return *this;
}

DirectoryBlobPartition::DirectoryBlobPartition(DirectoryBlobPartition&& other) noexcept
    : m_contents(std::move(other.m_contents)), m_root_directory(other.m_root_directory),
      m_is_wii(other.m_is_wii), m_is_triforce(other.m_is_triforce),
      m_address_shift(other.m_address_shift), m_data_size(other.m_data_size)
{
  std::memcpy(m_key, other.m_key, sizeof(m_key));
  other.m_root_directory = nullptr;
}

DirectoryBlobPartition& DirectoryBlobPartition::operator=(DirectoryBlobPartition&& other) noexcept
{
  if (this != &other)
  {
    std::free(m_root_directory);
    m_root_directory = other.m_root_directory;
    other.m_root_directory = nullptr;
    m_contents = std::move(other.m_contents);
    std::memcpy(m_key, other.m_key, sizeof(m_key));
    m_is_wii = other.m_is_wii;
    m_is_triforce = other.m_is_triforce;
    m_address_shift = other.m_address_shift;
    m_data_size = other.m_data_size;
  }
  return *this;
}

DirectoryBlobPartition::~DirectoryBlobPartition()
{
  std::free(m_root_directory);
}

void DirectoryBlobPartition::SetDiscType(const bool* is_wii, const u8* disc_header)
{
  if (is_wii)
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

void DirectoryBlobPartition::SetBI2FromFile(const char* bi2_path)
{
  ContentBlob* const blob = ZeroedBlob(BI2_SIZE);
  size_t bytes_read;
  if (!blob)
    return;

  if (!m_is_wii)
    Write32(INVALID_REGION, 0x18, ContentBlobData(blob));

  bytes_read = ReadFileToBuffer(bi2_path, ContentBlobData(blob), BI2_SIZE);
  if (!m_is_wii && bytes_read < 0x1C)
    ERROR_LOG(DISCIO, "Couldn't read region from %s", bi2_path);

  m_contents.Add(BI2_ADDRESS, BI2_SIZE, ContentMemory(blob, 0));
}

u64 DirectoryBlobPartition::SetApploaderFromFile(const char* path)
{
  File::DirectIOFile file(path, File::AccessMode::Read);
  const size_t size = size_t(file.GetSize());
  ContentBlob* blob = ContentBlobAlloc(size);
  if (blob && size && !file.OffsetRead(0, ContentBlobData(blob), size))
    blob->size = 0;
  return SetApploader(blob, path);
}

u64 DirectoryBlobPartition::SetApploader(ContentBlob* apploader, const char* log_path)
{
  bool success = false;
  const u64 size = apploader ? apploader->size : 0;

  if (size < 0x20)
  {
    ERROR_LOG(DISCIO, "%s couldn't be accessed or is too small", log_path);
  }
  else
  {
    const u8* const data = ContentBlobData(apploader);
    const u64 apploader_size = 0x20 + Common::swap32(data + 0x14) + Common::swap32(data + 0x18);
    if (apploader_size != size)
      ERROR_LOG(DISCIO, "%s is the wrong size... Is it really an apploader?", log_path);
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

u64 DirectoryBlobPartition::SetDOLFromFile(const char* path, u64 dol_address, u8* disc_header)
{
  const u64 dol_size = m_contents.CheckSizeAndAdd(dol_address, path);

  Write32(static_cast<u32>(dol_address >> m_address_shift), 0x0420, disc_header);

  // Return FST address, 32 byte aligned (plus 32 byte padding)
  return Common::AlignUp(dol_address + dol_size + 0x20, 0x20ull);
}

static void FreeFSTNodes(FSTBuilderNode* nodes, u32 count)
{
  u32 i;
  for (i = 0; i < count; i++)
  {
    std::free(nodes[i].m_filename);
    std::free(nodes[i].m_path);
    FreeFSTNodes(nodes[i].m_children, nodes[i].m_child_count);
  }
  std::free(nodes);
}

// The entries of the folder whose path is the length chars in path, a buffer of PATH_MAX_LENGTH
// that the entries' paths are built in; an entry whose path does not fit is left out. Each file
// gets its host path and size, and each folder its entries.
static FSTBuilderNode* ScanFSTFolder(char* path, size_t length, u32* count)
{
  FSTBuilderNode* nodes = nullptr;
  u32 capacity = 0;
  u32 i;
  struct RDIR* dir;

  *count = 0;
  path[length] = '\0';
  dir = retro_opendir_include_hidden(path, true);
  if (!dir)
    return nullptr;
  if (length && path[length - 1] != DIR_SEP_CHR)
  {
    if (length + 1 >= PATH_MAX_LENGTH)
    {
      retro_closedir(dir);
      return nullptr;
    }
    path[length++] = DIR_SEP_CHR;
  }

  while (retro_readdir(dir))
  {
    const char* const name = retro_dirent_get_name(dir);
    size_t name_length;
    FSTBuilderNode* node;
    if (!name || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
      continue;
    name_length = std::strlen(name);
    if (length + name_length >= PATH_MAX_LENGTH)
      continue;
    if (*count == capacity)
    {
      const u32 new_capacity = capacity ? capacity * 2 : 16;
      FSTBuilderNode* const grown =
          static_cast<FSTBuilderNode*>(std::realloc(nodes, new_capacity * sizeof(FSTBuilderNode)));
      if (!grown)
        break;
      nodes = grown;
      capacity = new_capacity;
    }
    std::memcpy(path + length, name, name_length + 1);
    node = &nodes[*count];
    node->m_filename = CopyPath(name);
    node->m_size = 0;
    node->m_path = nullptr;
    node->m_children = nullptr;
    node->m_child_count = 0;
    if (!node->m_filename)
      continue;
    if (!retro_dirent_is_dir(dir, nullptr))
    {
      int64_t size = 0;
      node->m_path = CopyPath(path);
      if (!node->m_path)
      {
        std::free(node->m_filename);
        continue;
      }
      if (retro_dirent_stat(dir, &size, nullptr) && size > 0)
        node->m_size = u64(size);
    }
    ++*count;
  }
  retro_closedir(dir);

  for (i = 0; i < *count; i++)
  {
    if (nodes[i].m_path)
      continue;
    const size_t name_length = std::strlen(nodes[i].m_filename);
    std::memcpy(path + length, nodes[i].m_filename, name_length + 1);
    nodes[i].m_children = ScanFSTFolder(path, length + name_length, &nodes[i].m_child_count);
  }
  return nodes;
}

void DirectoryBlobPartition::BuildFSTFromFolder(const char* fst_root_path, u64 fst_address,
                                                u8* disc_header)
{
  // the root without its trailing separators
  size_t length = std::strlen(fst_root_path);
  char* const path = static_cast<char*>(std::malloc(PATH_MAX_LENGTH));
  FSTBuilderNode* nodes = nullptr;
  u32 count = 0;
  while (length > 1 && fst_root_path[length - 1] == DIR_SEP_CHR)
    length--;
  if (path && length < PATH_MAX_LENGTH)
  {
    std::memcpy(path, fst_root_path, length);
    nodes = ScanFSTFolder(path, length, &count);
  }
  std::free(path);
  BuildFST(nodes, count, fst_address, disc_header);
}

static void ConvertUTF8NamesToSHIFTJIS(FSTBuilderNode* nodes, u32 count)
{
  u32 i;
  for (i = 0; i < count; i++)
  {
    FSTBuilderNode& entry = nodes[i];
    const size_t length = std::strlen(entry.m_filename);
    char* const converted = static_cast<char*>(std::malloc(length + 1));
    if (!entry.m_path)
      ConvertUTF8NamesToSHIFTJIS(entry.m_children, entry.m_child_count);
    if (!converted)
      continue;
    // at most an octet out per octet in
    converted[utf8_to_sjis(converted, length + 1, entry.m_filename, length)] = '\0';
    std::free(entry.m_filename);
    entry.m_filename = converted;
  }
}

static u32 ComputeNameSize(const FSTBuilderNode* files, u32 count)
{
  u32 name_size = 0;
  for (u32 i = 0; i < count; i++)
  {
    const FSTBuilderNode& entry = files[i];
    if (!entry.m_path)
      name_size += ComputeNameSize(entry.m_children, entry.m_child_count);
    name_size += static_cast<u32>(std::strlen(entry.m_filename) + 1);
  }
  return name_size;
}

// Upper-cased ASCII order, then plain order, compared in place.
static bool NameLess(const FSTBuilderNode& one, const FSTBuilderNode& two)
{
  const unsigned char* a = reinterpret_cast<const unsigned char*>(one.m_filename);
  const unsigned char* b = reinterpret_cast<const unsigned char*>(two.m_filename);
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
  return std::strcmp(one.m_filename, two.m_filename) < 0;
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
    const FSTBuilderNode moved = nodes[root];
    nodes[root] = nodes[child];
    nodes[child] = moved;
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
    const FSTBuilderNode moved = nodes[0];
    nodes[0] = nodes[i];
    nodes[i] = moved;
    SiftDown(nodes, 0, i);
  }
}

static size_t RecalculateFolderSizes(FSTBuilderNode* nodes, u32 count)
{
  size_t size = 0;
  for (u32 i = 0; i < count; i++)
  {
    FSTBuilderNode& entry = nodes[i];
    ++size;
    if (entry.m_path)
      continue;

    entry.m_size = RecalculateFolderSizes(entry.m_children, entry.m_child_count);
    size += entry.m_size;
  }
  return size;
}

void DirectoryBlobPartition::BuildFST(FSTBuilderNode* root_nodes, u32 root_count, u64 fst_address,
                                      u8* disc_header)
{
  ConvertUTF8NamesToSHIFTJIS(root_nodes, root_count);

  u32 name_table_size =
      Common::AlignUp(ComputeNameSize(root_nodes, root_count), 1ull << m_address_shift);

  // 1 extra for the root entry
  u64 total_entries = RecalculateFolderSizes(root_nodes, root_count) + 1;

  const u64 name_table_offset = total_entries * ENTRY_SIZE;
  // built in place, then added as it stands
  const u64 fst_size = name_table_offset + name_table_size;
  ContentBlob* const fst_blob = ZeroedBlob(size_t(fst_size));
  if (!fst_blob)
  {
    FreeFSTNodes(root_nodes, root_count);
    return;
  }
  u8* const fst_data = ContentBlobData(fst_blob);

  // 32 KiB aligned start of data on disc
  u64 current_data_address = Common::AlignUp(fst_address + fst_size, 0x8000ull);

  u32 fst_offset = 0;   // Offset within FST data
  u32 name_offset = 0;  // Offset within name table
  u32 root_offset = 0;  // Offset of root of FST

  // write root entry
  WriteEntryData(fst_data, &fst_offset, DIRECTORY_ENTRY, 0, 0, total_entries, m_address_shift);

  WriteDirectory(fst_data, root_nodes, root_count, &fst_offset, &name_offset, &current_data_address,
                 root_offset, name_table_offset);
  FreeFSTNodes(root_nodes, root_count);

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

void DirectoryBlobPartition::WriteEntryName(u8* fst_data, u32* name_offset, const char* name,
                                            u64 name_table_offset)
{
  const size_t size = std::strlen(name) + 1;
  std::memcpy(fst_data + *name_offset + name_table_offset, name, size);

  *name_offset += (u32)size;
}

void DirectoryBlobPartition::WriteDirectory(u8* fst_data, FSTBuilderNode* entries, u32 entry_count,
                                            u32* fst_offset, u32* name_offset, u64* data_offset,
                                            u32 parent_entry_index, u64 name_table_offset)
{
  // Sort for determinism
  SortByName(entries, entry_count);

  for (u32 i = 0; i < entry_count; i++)
  {
    FSTBuilderNode& entry = entries[i];
    if (!entry.m_path)
    {
      u32 entry_index = *fst_offset / ENTRY_SIZE;
      WriteEntryData(fst_data, fst_offset, DIRECTORY_ENTRY, *name_offset, parent_entry_index,
                     entry_index + entry.m_size + 1, 0);
      WriteEntryName(fst_data, name_offset, entry.m_filename, name_table_offset);

      WriteDirectory(fst_data, entry.m_children, entry.m_child_count, fst_offset, name_offset,
                     data_offset, entry_index, name_table_offset);
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
      m_contents.Add(*data_offset, entry.m_size, ContentFile(entry.m_path, 0));

      // For most files, an alignment of 0x20 is enough. But some files need 0x8000,
      // in particular all DTK audio files in GameCube games.
      // TODO: Do any Wii games need 0x8000 alignment?
      const u64 data_alignment = m_is_triforce ? 0x20ull : 0x8000ull;
      *data_offset = Common::AlignUp(*data_offset + entry.m_size, data_alignment);
    }
  }
}

// Up to size octets of the file at path into buffer; returns how many.
static size_t ReadFileToBuffer(const char* path, u8* buffer, size_t size)
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
