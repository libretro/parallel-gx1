// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CodePtrMap.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/RangeSet.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCAnalyst.h"

class JitBase;

// offsetof is only conditionally supported for non-standard layout types,
// so this struct needs to have a standard layout.
struct JitBlockData
{
  // Memory range this code block takes up in near and far code caches.
  u8* near_begin;
  u8* near_end;
  u8* far_begin;
  u8* far_end;

  // The normal entry point for the block, returned by Dispatch().
  u8* normalEntry;

  // The features that this block was compiled with support for.
  CPUEmuFeatureFlags feature_flags;
  // The effective address (PC) for the beginning of the block.
  u32 effectiveAddress;
  // The physical address of the code represented by this block.
  // Various maps in the cache are indexed by this (block_map
  // and valid_block in particular). This is useful because of
  // of the way the instruction cache works on PowerPC.
  u32 physicalAddress;
  // The number of PPC instructions represented by this block. Mostly
  // useful for logging.
  u32 originalSize;
  // This tracks the position of this block within the fast block cache.
  // We only allow each block to have one map entry.
  size_t fast_block_map_index;
};
static_assert(std::is_standard_layout_v<JitBlockData>, "JitBlockData must have a standard layout");

// A JitBlock is a block of compiled code which corresponds to the PowerPC
// code at a given address.
//
// The notion of the address of a block is a bit complicated because of the
// way address translation works, but basically it's the combination of an
// effective address, the address translation bits in MSR, and the physical
// address.
struct JitBlock : public JitBlockData
{
  bool OverlapsPhysicalRange(u32 address, u32 length) const;

  // Information about exits to a known address from this block.
  // This is used to implement block linking.
  struct LinkData
  {
    u8* exitPtrs;  // to be able to rewrite the exit jump
#ifdef _M_ARM_64
    const u8* exitFarcode;
#endif
    u32 exitAddress;
    bool linkStatus;  // is it already linked?
    bool call;

    // While the block is in the cache, the other exits to exitAddress, in a list the cache finds
    // by that address: owner is the block, null when this exit is in no list.
    JitBlock* owner = nullptr;
    LinkData* next_to = nullptr;
    LinkData* prev_to = nullptr;
  };
  std::vector<LinkData> linkData;

  // This set stores all physical addresses of all occupied instructions.
  Common::RangeSet<u32> physical_addresses;

  // The next block at the same physical address, in the cache's list of them; the next free one
  // while this one is free. A block goes back to the cache's pool when it is destroyed, keeping
  // linkData's and physical_addresses' storage for the next block made in its place.
  JitBlock* next_at_address = nullptr;
  bool in_use = false;
};

CODE_PTR_MAP_DEFINE(jit_link_lists, JitBlock::LinkData*);
CODE_PTR_MAP_DEFINE(jit_block_lists, JitBlock*);

typedef void (*CompiledCode)();

// This is essentially just an std::bitset, but Visual Studia 2013's
// implementation of std::bitset is slow.
class ValidBlockBitSet final
{
public:
  enum
  {
    // ValidBlockBitSet covers the whole 32-bit address-space in 32-byte
    // chunks.
    // FIXME: Maybe we can get away with less? There isn't any actual
    // RAM in most of this space.
    VALID_BLOCK_MASK_SIZE = (1ULL << 32) / 32,
    // The number of elements in the allocated array. Each u32 contains 32 bits.
    VALID_BLOCK_ALLOC_ELEMENTS = VALID_BLOCK_MASK_SIZE / 32
  };
  // Directly accessed by Jit64. 16 MiB of pages the system hands over zeroed and untouched
  // (calloc zeroes them itself where its heap has room): only the pages a block sets are written.
  u32* m_valid_block;
  // the words Set has written since the last ClearAll: [m_lo, m_hi)
  u32 m_lo;
  u32 m_hi;

  ValidBlockBitSet()
      : m_valid_block(static_cast<u32*>(
            Common::AllocateMemoryPages(VALID_BLOCK_ALLOC_ELEMENTS * sizeof(u32)))),
        m_lo(VALID_BLOCK_ALLOC_ELEMENTS), m_hi(0)
  {
  }
  ~ValidBlockBitSet()
  {
    if (m_valid_block)
      Common::FreeMemoryPages(m_valid_block, VALID_BLOCK_ALLOC_ELEMENTS * sizeof(u32));
  }
  ValidBlockBitSet(const ValidBlockBitSet&) = delete;
  ValidBlockBitSet& operator=(const ValidBlockBitSet&) = delete;

  void Set(u32 bit)
  {
    const u32 word = bit / 32;
    m_valid_block[word] |= 1u << (bit % 32);
    if (word < m_lo)
      m_lo = word;
    if (word >= m_hi)
      m_hi = word + 1;
  }
  void Clear(u32 bit) { m_valid_block[bit / 32] &= ~(1u << (bit % 32)); }
  /* only the words set since the last: the rest are zero already */
  void ClearAll()
  {
    if (m_lo < m_hi)
      memset(m_valid_block + m_lo, 0, sizeof(u32) * (m_hi - m_lo));
    m_lo = VALID_BLOCK_ALLOC_ELEMENTS;
    m_hi = 0;
  }
  bool Test(u32 bit) const { return (m_valid_block[bit / 32] & (1u << (bit % 32))) != 0; }
};

class JitBaseBlockCache
{
public:
  // The size of the fast map is determined like this:
  // ((4 GiB guest memory space) / (4-byte alignment) * sizeof(JitBlock*)) << (3 feature flag bits)
  static constexpr u64 FAST_BLOCK_MAP_SIZE = 0x10'0000'0000;
  static constexpr u32 FAST_BLOCK_MAP_FALLBACK_ELEMENTS = 0x10000;
  static constexpr u32 FAST_BLOCK_MAP_FALLBACK_MASK = FAST_BLOCK_MAP_FALLBACK_ELEMENTS - 1;

  explicit JitBaseBlockCache(JitBase& jit);
  virtual ~JitBaseBlockCache();

  virtual void Init();
  void Shutdown();
  void Clear();
  void Reset();

  // Code Cache
  u8** GetEntryPoints();
  JitBlock** GetFastBlockMapFallback();

  JitBlock* AllocateBlock(u32 em_address);
  void FinalizeBlock(JitBlock& block, bool block_link, const PPCAnalyst::CodeBlock& code_block,
                     const PPCAnalyst::CodeBuffer& code_buffer);

  // Look for the block in the slow but accurate way.
  // This function shall be used if FastLookupIndexForAddress() failed.
  // This might return nullptr if there is no such block.
  JitBlock* GetBlockFromStartAddress(u32 em_address, CPUEmuFeatureFlags feature_flags);

  // Get the normal entry for the block associated with the current program
  // counter. This will JIT code if necessary. (This is the reference
  // implementation; high-performance JITs will want to use a custom
  // assembly version.)
  const u8* Dispatch();

  void InvalidateICache(u32 address, u32 length, bool forced);
  void InvalidateICacheLine(u32 address);
  void ErasePhysicalRange(u32 address, u32 length);

  u32* GetBlockBitSet() const;

protected:
  virtual void DestroyBlock(JitBlock& block);

  JitBase& m_jit;

private:
  virtual void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) = 0;
  virtual void WriteDestroyBlock(const JitBlock& block);

  void LinkBlockExits(JitBlock& block);
  void LinkBlock(JitBlock& block);
  void UnlinkBlock(const JitBlock& block);
  void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length, bool forced);

  JitBlock* MoveBlockIntoFastCache(u32 em_address, CPUEmuFeatureFlags feature_flags);

  // Fast but risky block lookup based on fast_block_map.
  size_t FastLookupIndexForAddress(u32 address, u32 msr);

  JitBlock* TakeBlock();
  void ReleaseBlock(JitBlock* block);

  // links_to hold all exit points of all valid blocks in a reverse way.
  // It is used to query all blocks which links to an address: by the destination PC, the list of
  // exits to it (LinkData::next_to).
  jit_link_lists_t links_to;

  // The blocks by the physical address of the entry point, each a list in the order they were made
  // (JitBlock::next_at_address). This is used to query the block based on the current PC in a
  // slow way.
  jit_block_lists_t block_map;

  // Where the blocks live: chunks of BLOCK_CHUNK, never given back while the cache is, and the
  // free ones in a list. A block made where one was destroyed allocates nothing.
  static constexpr size_t BLOCK_CHUNK = 1024;
  JitBlock** m_block_chunks = nullptr;
  size_t m_num_block_chunks = 0;
  size_t m_block_chunks_cap = 0;
  JitBlock* m_free_blocks = nullptr;

  // Range of overlapping code indexed by a masked physical address.
  // This is used for invalidation of memory regions. The range is grouped
  // in macro blocks of each 0x100 bytes.
  static constexpr u32 BLOCK_RANGE_SIZE = 0x100;
  static constexpr u32 BLOCK_RANGE_MAP_MASK = ~(BLOCK_RANGE_SIZE - 1);
  std::map<u32, std::unordered_set<JitBlock*>> block_range_map;

  // This bitsets shows which cachelines overlap with any blocks.
  // It is used to provide a fast way to query if no icache invalidation is needed.
  ValidBlockBitSet valid_block;

  // This contains the entry points for each block.
  // It is used by the assembly dispatcher to quickly
  // know where to jump based on pc and msr bits.
  Common::LazyMemoryRegion m_entry_points_arena;
  u8** m_entry_points_ptr = nullptr;

  // An alternative for the above but without a shm segment
  // in case the shm memory region couldn't be allocated.
  std::array<JitBlock*, FAST_BLOCK_MAP_FALLBACK_ELEMENTS>
      m_fast_block_map_fallback{};  // start_addr & mask -> number
};
