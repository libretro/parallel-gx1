// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// BLOB

// Blobs in Dolphin are read only Binary Large OBjects. For example, a typical DVD image.
// Often, you may want to store these things in a highly compressed format, but still
// allow random access. Or you may store them on an odd device, like raw on a DVD.

// Always read your BLOBs using an interface returned by CreateBlobReader(). It will
// detect whether the file is a compressed blob, or just a big hunk of data, or a drive, and
// automatically do the right thing.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace DiscIO
{
enum class BlobType
{
  PLAIN,
  DRIVE,
  DIRECTORY,
  GCZ,
  CISO,
  WBFS,
  TGC,
  WIA,
  RVZ,
  NFS,
  SPLIT_PLAIN,
};

// If you convert an ISO file to another format and then call GetDataSize on it, what is the result?
enum class DataSizeType
{
  // The result is the same as for the ISO.
  Accurate,
  // The result is not larger than for the ISO. (It's usually a little smaller than for the ISO.)
  // Reads to offsets that are larger than the result will return some kind of "blank" data.
  LowerBound,
  // The result is not smaller than for the ISO. (It's usually much larger than for the ISO.)
  UpperBound,
};

std::string GetName(BlobType blob_type, bool translate);

// The class a BlobReader is, one for each format and for each reader made over another one
// (scrubbed, cached, a file in a volume). GetBlobType tells the format, which a reader over
// another one shares with it.
enum class BlobReaderKind : u8
{
  Plain,
  SplitPlain,
  Directory,
  Compressed,
  CISO,
  Wbfs,
  TGC,
  WIARVZ,
  NFS,
  Scrubbed,
  Cached,
  VolumeFile,
};

// The functions of a reader go to its own class through a branch on its kind, not a table of
// virtual functions; the ones with a default here go to its class only where it has its own.
// One is owned through a plain pointer: what makes or copies one hands it to the caller, which
// frees it with DestroyBlobReader, as its own class.
class BlobReader
{
public:
  BlobReader(const BlobReader&) = delete;
  BlobReader& operator=(const BlobReader&) = delete;

  BlobReaderKind GetKind() const { return m_kind; }

  BlobType GetBlobType() const;
  BlobReader* CopyReader() const;

  u64 GetRawSize() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;

  // Returns 0 if the format does not use blocks
  u64 GetBlockSize() const;
  bool HasFastRandomAccessInBlock() const;
  std::string GetCompressionMethod() const;
  std::optional<int> GetCompressionLevel() const;

  // NOT thread-safe - can't call this from multiple threads.
  bool Read(u64 offset, u64 size, u8* out_ptr);
  template <typename T>
  std::optional<T> ReadSwapped(u64 offset)
  {
    T temp;
    if (!Read(offset, sizeof(T), reinterpret_cast<u8*>(&temp)))
      return std::nullopt;
    return Common::FromBigEndian(temp);
  }

  // false where the class has none of its own
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset);

  // size octets at offset as they lie in the file, lent rather than copied, valid while
  // this reader lives; nullptr where the format or the file cannot lend them. Thread-safe.
  const u8* View(u64 offset, u64 size) const;
  // Asks the OS to start reading what View(offset, size) lends; returns at once.
  void Prefetch(u64 offset, u64 size) const;

  // Returns true only for CachedBlobReader.
  bool IsCached() const;

protected:
  // not virtual: DestroyBlobReader destroys a reader as the class it is
  ~BlobReader() = default;

  explicit BlobReader(BlobReaderKind kind) : m_kind(kind) {}

private:
  const BlobReaderKind m_kind;
};

// The base of a reader of kind K
template <BlobReaderKind K>
class BlobReaderOfKind : public BlobReader
{
protected:
  BlobReaderOfKind() : BlobReader(K) {}
  ~BlobReaderOfKind() = default;
};

// Provides caching and byte-operation-to-block-operations facilities.
// Used for compressed blob and direct drive reading.
// NOTE: GetDataSize() is expected to be evenly divisible by the sector size.
class SectorReader : public BlobReaderOfKind<BlobReaderKind::Compressed>
{
public:
  bool Read(u64 offset, u64 size, u8* out_ptr);

protected:
  ~SectorReader();

protected:
  void SetSectorSize(int blocksize);
  int GetSectorSize() const { return m_block_size; }
  // Set the chunk size -> the number of blocks to read at a time.
  // Default value is 1 but that is too low for physical devices
  // like CDROMs. Setting this to a higher value helps reduce seeking
  // and IO overhead by batching reads. Do not set it too high either
  // as large reads are slow and will take too long to resolve.
  int GetChunkSize() const { return m_chunk_blocks; }
  // Read a single block/sector: the reader's own (the GCZ reader is the only SectorReader)
  bool GetBlock(u64 block_num, u8* out);

  // Read multiple contiguous blocks.
  // Default implementation just calls GetBlock in a loop, it should be
  // overridden in derived classes where possible.
  bool ReadMultipleAlignedBlocks(u64 block_num, u64 num_blocks, u8* out_ptr);

private:
  struct Cache
  {
    std::vector<u8> data;
    u64 block_idx = 0;
    u32 num_blocks = 0;

    // [Pseudo-] Least Recently Used Shift Register
    // When an empty cache line is needed, the line with the lowest value
    // is taken and reset; the LRU register is then shifted down 1 place
    // on all lines (low bit discarded). When a line is used, the high bit
    // is set marking it as most recently used.
    u32 lru_sreg = 0;

    void Reset()
    {
      block_idx = 0;
      num_blocks = 0;
      lru_sreg = 0;
    }
    void Fill(u64 block, u32 count)
    {
      block_idx = block;
      num_blocks = count;
      // NOTE: Setting only the high bit means the newest line will
      //   be selected for eviction if every line in the cache was
      //   touched. This gives MRU behavior which is probably
      //   desirable in that case.
      MarkUsed();
    }
    bool Contains(u64 block) const { return block >= block_idx && block - block_idx < num_blocks; }
    void MarkUsed() { lru_sreg |= 0x80000000; }
    void ShiftLRU() { lru_sreg >>= 1; }
    bool IsLessRecentlyUsedThan(const Cache& other) const { return lru_sreg < other.lru_sreg; }
  };

  // Gets the cache line that contains the given block, or nullptr.
  // NOTE: The cache record only lasts until it expires (next GetEmptyCacheLine)
  const Cache* FindCacheLine(u64 block_num);

  // Finds the least recently used cache line, resets and returns it.
  Cache* GetEmptyCacheLine();

  // Combines FindCacheLine with GetEmptyCacheLine and ReadChunk.
  // Always returns a valid cache line (loading the data if needed).
  // May return nullptr only if the cache missed and the read failed.
  const Cache* GetCacheLine(u64 block_num);

  // Read all bytes from a chunk of blocks into a buffer.
  // Returns the number of blocks read (may be less than m_chunk_blocks
  // if chunk_num is the last chunk on the disk and the disk size is not
  // evenly divisible into chunks). Returns zero if it fails.
  u32 ReadChunk(u8* buffer, u64 chunk_num);

  static constexpr int CACHE_LINES = 32;
  u32 m_block_size = 0;    // Bytes in a sector/block
  u32 m_chunk_blocks = 1;  // Number of sectors/blocks in a chunk
  std::array<Cache, CACHE_LINES> m_cache;
};

// Factory function - examines the path to choose the right type of BlobReader, and returns one.
// Owned by the caller, nullptr if the file cannot be read as a disc
BlobReader* CreateBlobReader(const std::string& filename);

// Destroys a reader as the class it is and frees it; nullptr does nothing.
void DestroyBlobReader(BlobReader* reader);

}  // namespace DiscIO
