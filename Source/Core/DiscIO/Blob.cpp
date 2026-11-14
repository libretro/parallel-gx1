// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/Blob.h"

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "Common/MsgHandler.h"

#include "DiscIO/CISOBlob.h"
#include "DiscIO/CachedBlob.h"
#include "DiscIO/CompressedBlob.h"
#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/FileBlob.h"
#include "DiscIO/NFSBlob.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/SplitFileBlob.h"
#include "DiscIO/TGCBlob.h"
#include "DiscIO/VolumeFileBlobReader.h"
#include "DiscIO/WIABlob.h"
#include "DiscIO/WbfsBlob.h"

namespace DiscIO
{
// f called with the reader as the class it is
template <typename Reader, typename F>
static decltype(auto) AsOwnClass(Reader* reader, F&& f)
{
  constexpr bool is_const = std::is_const_v<Reader>;
#define AS_KIND(kind, cls)                                                                         \
  case BlobReaderKind::kind:                                                                       \
    return f(static_cast<std::conditional_t<is_const, const cls*, cls*>>(reader));
  switch (reader->GetKind())
  {
    AS_KIND(Plain, PlainFileReader)
    AS_KIND(SplitPlain, SplitPlainFileReader)
    AS_KIND(Directory, DirectoryBlobReader)
    AS_KIND(Compressed, CompressedBlobReader)
    AS_KIND(CISO, CISOFileReader)
    AS_KIND(Wbfs, WbfsFileReader)
    AS_KIND(TGC, TGCFileReader)
    AS_KIND(WIARVZ, WIARVZFileReader)
    AS_KIND(NFS, NFSFileReader)
    AS_KIND(Scrubbed, ScrubbedBlob)
    AS_KIND(Cached, CachedBlobReader)
  case BlobReaderKind::VolumeFile:
  default:
    return f(static_cast<
             std::conditional_t<is_const, const VolumeFileBlobReader*, VolumeFileBlobReader*>>(
        reader));
  }
#undef AS_KIND
}

// Whether a class has its own Name rather than BlobReader's default
#define HAS_OWN(cls, Name)                                                                         \
  (!std::is_same_v<decltype(&std::remove_cv_t<cls>::Name), decltype(&BlobReader::Name)>)

BlobType BlobReader::GetBlobType() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetBlobType(); });
}

BlobReader* BlobReader::CopyReader() const
{
  return AsOwnClass(this, [](auto* r) { return r->CopyReader(); });
}

u64 BlobReader::GetRawSize() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetRawSize(); });
}

u64 BlobReader::GetDataSize() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetDataSize(); });
}

DataSizeType BlobReader::GetDataSizeType() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetDataSizeType(); });
}

u64 BlobReader::GetBlockSize() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetBlockSize(); });
}

bool BlobReader::HasFastRandomAccessInBlock() const
{
  return AsOwnClass(this, [](auto* r) { return r->HasFastRandomAccessInBlock(); });
}

std::string BlobReader::GetCompressionMethod() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetCompressionMethod(); });
}

std::optional<int> BlobReader::GetCompressionLevel() const
{
  return AsOwnClass(this, [](auto* r) { return r->GetCompressionLevel(); });
}

bool BlobReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  return AsOwnClass(this, [&](auto* r) { return r->Read(offset, size, out_ptr); });
}

bool BlobReader::SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const
{
  return AsOwnClass(this, [&](auto* r) {
    using Cls = std::remove_pointer_t<decltype(r)>;
    if constexpr (HAS_OWN(Cls, SupportsReadWiiDecrypted))
      return r->SupportsReadWiiDecrypted(offset, size, partition_data_offset);
    else
      return false;
  });
}

bool BlobReader::ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset)
{
  return AsOwnClass(this, [&](auto* r) {
    using Cls = std::remove_pointer_t<decltype(r)>;
    if constexpr (HAS_OWN(Cls, ReadWiiDecrypted))
      return r->ReadWiiDecrypted(offset, size, out_ptr, partition_data_offset);
    else
      return false;
  });
}

const u8* BlobReader::View(u64 offset, u64 size) const
{
  return AsOwnClass(this, [&](auto* r) -> const u8* {
    using Cls = std::remove_pointer_t<decltype(r)>;
    if constexpr (HAS_OWN(Cls, View))
      return r->View(offset, size);
    else
      return nullptr;
  });
}

void BlobReader::Prefetch(u64 offset, u64 size) const
{
  AsOwnClass(this, [&](auto* r) {
    using Cls = std::remove_pointer_t<decltype(r)>;
    if constexpr (HAS_OWN(Cls, Prefetch))
      r->Prefetch(offset, size);
  });
}

bool BlobReader::IsCached() const
{
  return AsOwnClass(this, [](auto* r) {
    using Cls = std::remove_pointer_t<decltype(r)>;
    if constexpr (HAS_OWN(Cls, IsCached))
      return r->IsCached();
    else
      return false;
  });
}

void DestroyBlobReader(BlobReader* reader)
{
  if (!reader)
    return;
  AsOwnClass(reader, [](auto* r) { delete r; });
}

bool SectorReader::GetBlock(u64 block_num, u8* out)
{
  return static_cast<CompressedBlobReader*>(this)->GetBlock(block_num, out);
}

std::string GetName(BlobType blob_type, bool translate)
{
  const auto translate_str = [translate](const std::string& str) {
    return translate ? Common::GetStringT(str.c_str()) : str;
  };

  switch (blob_type)
  {
  case BlobType::PLAIN:
    return "ISO";
  case BlobType::DIRECTORY:
    return translate_str("Directory");
  case BlobType::GCZ:
    return "GCZ";
  case BlobType::CISO:
    return "CISO";
  case BlobType::WBFS:
    return "WBFS";
  case BlobType::TGC:
    return "TGC";
  case BlobType::WIA:
    return "WIA";
  case BlobType::RVZ:
    return "RVZ";
  case BlobType::NFS:
    return "NFS";
  case BlobType::SPLIT_PLAIN:
    return translate_str("Multi-part ISO");
  default:
    return "";
  }
}

void SectorReader::SetSectorSize(int blocksize)
{
  m_block_size = blocksize > 0 ? blocksize : 0;
  for (auto& cache_entry : m_cache)
  {
    cache_entry.Reset();
    cache_entry.data.resize(m_chunk_blocks * m_block_size);
  }
}

SectorReader::~SectorReader() = default;

const SectorReader::Cache* SectorReader::FindCacheLine(u64 block_num)
{
  for (Cache& entry : m_cache)
  {
    if (!entry.Contains(block_num))
      continue;
    entry.MarkUsed();
    return &entry;
  }
  return nullptr;
}

SectorReader::Cache* SectorReader::GetEmptyCacheLine()
{
  Cache* oldest = &m_cache[0];
  // Find the Least Recently Used cache line to replace.
  for (size_t i = 1; i < m_cache.size(); i++)
  {
    Cache& line = m_cache[i];
    if (line.IsLessRecentlyUsedThan(*oldest))
    {
      oldest->ShiftLRU();
      oldest = &line;
      continue;
    }
    line.ShiftLRU();
  }
  oldest->Reset();
  return oldest;
}

const SectorReader::Cache* SectorReader::GetCacheLine(u64 block_num)
{
  if (auto entry = FindCacheLine(block_num))
    return entry;

  // Cache miss. Fault in the missing entry.
  Cache* cache = GetEmptyCacheLine();
  // We only read aligned chunks, this avoids duplicate overlapping entries.
  u64 chunk_idx = block_num / m_chunk_blocks;
  u32 blocks_read = ReadChunk(cache->data.data(), chunk_idx);
  if (!blocks_read)
    return nullptr;
  cache->Fill(chunk_idx * m_chunk_blocks, blocks_read);

  // Secondary check for out-of-bounds read.
  // If we got less than m_chunk_blocks, we may still have missed.
  // We do this after the cache fill since the cache line itself is
  // fine, the problem is being asked to read past the end of the disk.
  return cache->Contains(block_num) ? cache : nullptr;
}

bool SectorReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  if (offset + size > GetDataSize())
    return false;

  u64 remain = size;
  u64 block = 0;
  u32 position_in_block = static_cast<u32>(offset % m_block_size);

  while (remain > 0)
  {
    block = offset / m_block_size;

    const Cache* cache = GetCacheLine(block);
    if (!cache)
      return false;

    // Cache entries are aligned chunks, we may not want to read from the start
    u32 read_offset = static_cast<u32>(block - cache->block_idx) * m_block_size + position_in_block;
    u32 can_read = m_block_size * cache->num_blocks - read_offset;
    u32 was_read = static_cast<u32>(can_read < remain ? u64(can_read) : remain);

    std::memcpy(out_ptr, cache->data.data() + read_offset, was_read);

    offset += was_read;
    out_ptr += was_read;
    remain -= was_read;
    position_in_block = 0;
  }
  return true;
}

// Crap default implementation if not overridden.
bool SectorReader::ReadMultipleAlignedBlocks(u64 block_num, u64 cnt_blocks, u8* out_ptr)
{
  for (u64 i = 0; i < cnt_blocks; ++i)
  {
    if (!GetBlock(block_num + i, out_ptr))
      return false;
    out_ptr += m_block_size;
  }
  return true;
}

u32 SectorReader::ReadChunk(u8* buffer, u64 chunk_num)
{
  u64 block_num = chunk_num * m_chunk_blocks;
  u32 cnt_blocks = m_chunk_blocks;

  // If we are reading the end of a disk, there may not be enough blocks to
  // read a whole chunk. We need to clamp down in that case.
  u64 end_block = (GetDataSize() + m_block_size - 1) / m_block_size;
  if (end_block)
  {
    const u64 blocks_left = end_block - block_num;
    cnt_blocks = static_cast<u32>(u64(m_chunk_blocks) < blocks_left ? u64(m_chunk_blocks) :
                                                                        blocks_left);
  }

  if (ReadMultipleAlignedBlocks(block_num, cnt_blocks, buffer))
  {
    if (cnt_blocks < m_chunk_blocks)
    {
      std::memset(buffer + cnt_blocks * m_block_size, 0,
                  (m_chunk_blocks - cnt_blocks) * m_block_size);
    }
    return cnt_blocks;
  }

  // end_block may be zero on real disks if we fail to get the media size.
  // We have to fallback to probing the disk instead.
  if (!end_block)
  {
    for (u32 i = 0; i < cnt_blocks; ++i)
    {
      if (!GetBlock(block_num + i, buffer))
      {
        std::memset(buffer, 0, (cnt_blocks - i) * m_block_size);
        return i;
      }
      buffer += m_block_size;
    }
    return cnt_blocks;
  }
  return 0;
}

BlobReader* CreateBlobReader(const std::string& filename)
{
  File::DirectIOFile file(filename, File::AccessMode::Read);
  u32 magic;
  if (!file.Read(reinterpret_cast<u8*>(&magic), sizeof(magic)))
    return nullptr;

  // Conveniently, every supported file format (except for plain disc images and
  // extracted discs) starts with a 4-byte magic number that identifies the format,
  // so we just need a simple switch statement to create the right blob type. If the
  // magic number doesn't match any known magic number and the directory structure
  // doesn't match the directory blob format, we assume it's a plain disc image. If
  // that assumption is wrong, the volume code that runs later will notice the error
  // because the blob won't provide the right data when reading the GC/Wii disc header.

  switch (magic)
  {
  case CISO_MAGIC:
    return CISOFileReader::Create(std::move(file));
  case GCZ_MAGIC:
    return CompressedBlobReader::Create(std::move(file), filename);
  case TGC_MAGIC:
    return TGCFileReader::Create(std::move(file));
  case WBFS_MAGIC:
    return WbfsFileReader::Create(std::move(file), filename);
  case WIA_MAGIC:
  case RVZ_MAGIC:
    return WIARVZFileReader::Create(std::move(file), filename);
  case NFS_MAGIC:
    return NFSFileReader::Create(std::move(file), filename);
  default:
    if (BlobReader* directory_blob = DirectoryBlobReader::Create(filename))
      return directory_blob;
    if (BlobReader* split_blob = SplitPlainFileReader::Create(filename))
      return split_blob;

    return PlainFileReader::Create(std::move(file));
  }
}

}  // namespace DiscIO
