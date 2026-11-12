// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitCommon/JitCache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace Gen;

/* An address as the key of a code address map */
static const void* AddressKey(u32 address)
{
  return reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
}

bool JitBlock::OverlapsPhysicalRange(u32 address, u32 length) const
{
  return physical_addresses.overlaps(address, address + length);
}

JitBaseBlockCache::JitBaseBlockCache(JitBase& jit) : m_jit{jit}
{
  jit_link_lists_init(&links_to, 4096);
  jit_block_lists_init(&block_map, 8192);
  jit_range_lists_init(&block_range_map, 8192);
}

JitBaseBlockCache::~JitBaseBlockCache()
{
  size_t i;
  for (i = 0; i < m_num_block_chunks; i++)
    delete[] m_block_chunks[i];
  free(m_block_chunks);
  for (i = 0; i < m_num_range_node_chunks; i++)
    free(m_range_node_chunks[i]);
  free(m_range_node_chunks);
  jit_range_lists_free(&block_range_map);
  jit_block_lists_free(&block_map);
  jit_link_lists_free(&links_to);
}

JitBlock* JitBaseBlockCache::TakeBlock()
{
  JitBlock* block = m_free_blocks;
  if (!block)
  {
    JitBlock* chunk;
    size_t i;
    if (m_num_block_chunks == m_block_chunks_cap)
    {
      const size_t cap = m_block_chunks_cap ? m_block_chunks_cap * 2 : 16;
      JitBlock** const chunks =
          static_cast<JitBlock**>(realloc(m_block_chunks, cap * sizeof(*chunks)));
      if (!chunks)
        abort();
      m_block_chunks = chunks;
      m_block_chunks_cap = cap;
    }
    chunk = new JitBlock[BLOCK_CHUNK];
    m_block_chunks[m_num_block_chunks++] = chunk;
    for (i = BLOCK_CHUNK; i-- > 0;)
    {
      chunk[i].next_at_address = m_free_blocks;
      m_free_blocks = &chunk[i];
    }
    block = m_free_blocks;
  }
  m_free_blocks = block->next_at_address;
  block->next_at_address = nullptr;
  block->in_use = true;
  return block;
}

void JitBaseBlockCache::AddToMacroBlock(JitBlock* block, u32 macro_block)
{
  JitRangeNode* node;
  JitRangeNode** head;
  /* once in each */
  for (node = block->range_nodes; node; node = node->next_of_block)
  {
    if (node->macro_block == macro_block)
      return;
  }
  if (!m_free_range_nodes)
  {
    JitRangeNode* chunk;
    size_t i;
    if (m_num_range_node_chunks == m_range_node_chunks_cap)
    {
      const size_t cap = m_range_node_chunks_cap ? m_range_node_chunks_cap * 2 : 16;
      JitRangeNode** const chunks =
          static_cast<JitRangeNode**>(realloc(m_range_node_chunks, cap * sizeof(*chunks)));
      if (!chunks)
        abort();
      m_range_node_chunks = chunks;
      m_range_node_chunks_cap = cap;
    }
    chunk = static_cast<JitRangeNode*>(malloc(RANGE_NODE_CHUNK * sizeof(*chunk)));
    if (!chunk)
      abort();
    m_range_node_chunks[m_num_range_node_chunks++] = chunk;
    for (i = 0; i < RANGE_NODE_CHUNK; i++)
    {
      chunk[i].next_of_block = m_free_range_nodes;
      m_free_range_nodes = &chunk[i];
    }
  }
  node = m_free_range_nodes;
  m_free_range_nodes = node->next_of_block;

  head = jit_range_lists_get(&block_range_map, AddressKey(macro_block));
  node->block = block;
  node->macro_block = macro_block;
  node->prev_in_range = nullptr;
  node->next_in_range = *head;
  if (*head)
    (*head)->prev_in_range = node;
  *head = node;
  node->next_of_block = block->range_nodes;
  block->range_nodes = node;
}

void JitBaseBlockCache::LeaveMacroBlocks(JitBlock* block)
{
  JitRangeNode* node = block->range_nodes;
  while (node)
  {
    JitRangeNode* const next = node->next_of_block;
    if (node->prev_in_range)
    {
      node->prev_in_range->next_in_range = node->next_in_range;
    }
    else
    {
      JitRangeNode** const head =
          jit_range_lists_find(&block_range_map, AddressKey(node->macro_block));
      if (head && *head == node)
        *head = node->next_in_range;
    }
    if (node->next_in_range)
      node->next_in_range->prev_in_range = node->prev_in_range;
    node->next_of_block = m_free_range_nodes;
    m_free_range_nodes = node;
    node = next;
  }
  block->range_nodes = nullptr;
}

void JitBaseBlockCache::ReleaseBlock(JitBlock* block)
{
  block->in_use = false;
  block->next_at_address = m_free_blocks;
  m_free_blocks = block;
}

void JitBaseBlockCache::Init()
{
  Common::JitRegister::Init(Config::Get(Config::MAIN_PERF_MAP_DIR));

  m_entry_points_ptr = nullptr;
#if defined(_ARCH_64) && !defined(IPHONEOS)
  if (Config::Get(Config::MAIN_LARGE_ENTRY_POINTS_MAP))
    m_entry_points_ptr = static_cast<u8**>(m_entry_points_arena.Create(FAST_BLOCK_MAP_SIZE));
#endif

  Clear();
}

void JitBaseBlockCache::Shutdown()
{
  Common::JitRegister::Shutdown();

  m_entry_points_arena.Release();
}

// This clears the JIT cache. It's called from JitCache.cpp when the JIT cache
// is full and when saving and loading states.
void JitBaseBlockCache::Clear()
{
#if defined(_DEBUG) || defined(DEBUGFAST)
  Core::DisplayMessage("Clearing code cache.", 3000);
#endif
  m_jit.js.fifoWriteAddresses.clear();
  m_jit.js.pairedQuantizeAddresses.clear();
  m_jit.js.noSpeculativeConstantsAddresses.clear();
  size_t chunk, i;
  for (chunk = 0; chunk < m_num_block_chunks; chunk++)
  {
    for (i = 0; i < BLOCK_CHUNK; i++)
    {
      if (m_block_chunks[chunk][i].in_use)
        DestroyBlock(m_block_chunks[chunk][i]);
    }
  }
  for (chunk = 0; chunk < m_num_block_chunks; chunk++)
  {
    for (i = 0; i < BLOCK_CHUNK; i++)
    {
      if (m_block_chunks[chunk][i].in_use)
      {
        LeaveMacroBlocks(&m_block_chunks[chunk][i]);
        ReleaseBlock(&m_block_chunks[chunk][i]);
      }
    }
  }
  jit_block_lists_clear(&block_map);
  jit_link_lists_clear(&links_to);
  jit_range_lists_clear(&block_range_map);

  valid_block.ClearAll();

  if (m_entry_points_ptr)
    m_entry_points_arena.Clear();
}

void JitBaseBlockCache::Reset()
{
  Shutdown();
  Init();
}

u8** JitBaseBlockCache::GetEntryPoints()
{
  return m_entry_points_ptr;
}

JitBlock** JitBaseBlockCache::GetFastBlockMapFallback()
{
  return m_fast_block_map_fallback.data();
}

JitBlock* JitBaseBlockCache::AllocateBlock(u32 em_address)
{
  const u32 physical_address = m_jit.m_mmu.JitCache_TranslateAddress(em_address).address;
  JitBlock& b = *TakeBlock();
  JitBlock** at = jit_block_lists_get(&block_map, AddressKey(physical_address));
  /* last in its address's list, as a multimap kept it */
  while (*at)
    at = &(*at)->next_at_address;
  *at = &b;
  static_cast<JitBlockData&>(b) = JitBlockData{};
  b.effectiveAddress = em_address;
  b.physicalAddress = physical_address;
  b.feature_flags = m_jit.m_ppc_state.feature_flags;
  b.linkData.clear();
  b.fast_block_map_index = 0;
  return &b;
}

void JitBaseBlockCache::FinalizeBlock(JitBlock& block, bool block_link,
                                      const PPCAnalyst::CodeBlock& code_block,
                                      const PPCAnalyst::CodeBuffer& code_buffer)
{
  size_t index = FastLookupIndexForAddress(block.effectiveAddress, block.feature_flags);
  if (m_entry_points_ptr)
  {
    m_entry_points_arena.EnsureMemoryPageWritable(index * sizeof(u8*));
    m_entry_points_ptr[index] = block.normalEntry;
  }
  else
  {
    m_fast_block_map_fallback[index] = &block;
  }
  block.fast_block_map_index = index;

  block.physical_addresses = code_block.m_physical_addresses;

  block.originalSize = code_block.m_num_instructions;
  for (auto [range_start, range_end] : block.physical_addresses)
  {
    for (u32 i = range_start & ~31; i < range_end; i += 32)
      valid_block.Set(i / 32);

    for (u32 i = range_start & BLOCK_RANGE_MAP_MASK; i < range_end; i += BLOCK_RANGE_SIZE)
      AddToMacroBlock(&block, i);
  }

  if (block_link)
  {
    for (auto& e : block.linkData)
    {
      JitBlock::LinkData** const head = jit_link_lists_get(&links_to, AddressKey(e.exitAddress));
      e.owner = &block;
      e.prev_to = nullptr;
      e.next_to = *head;
      if (*head)
        (*head)->prev_to = &e;
      *head = &e;
    }

    LinkBlock(block);
  }

  const Common::Symbol* symbol = nullptr;
  if (Common::JitRegister::IsEnabled() &&
      (symbol = m_jit.m_ppc_symbol_db.GetSymbolFromAddr(block.effectiveAddress)) != nullptr)
  {
    Common::JitRegister::Register(block.normalEntry, block.near_end, "JIT_PPC_%s_%08x",
                                  symbol->function_name.c_str(), block.physicalAddress);
  }
  else
  {
    Common::JitRegister::Register(block.normalEntry, block.near_end, "JIT_PPC_%08x",
                                  block.physicalAddress);
  }
}

JitBlock* JitBaseBlockCache::GetBlockFromStartAddress(u32 addr, CPUEmuFeatureFlags feature_flags)
{
  u32 translated_addr = addr;
  if (feature_flags & FEATURE_FLAG_MSR_IR)
  {
    auto translated = m_jit.m_mmu.JitCache_TranslateAddress(addr);
    if (!translated.valid)
    {
      return nullptr;
    }
    translated_addr = translated.address;
  }

  JitBlock* const* const head = jit_block_lists_find(&block_map, AddressKey(translated_addr));
  for (JitBlock* b = head ? *head : nullptr; b; b = b->next_at_address)
  {
    if (b->effectiveAddress == addr && b->feature_flags == feature_flags)
      return b;
  }

  return nullptr;
}

const u8* JitBaseBlockCache::Dispatch()
{
  const auto& ppc_state = m_jit.m_ppc_state;
  if (m_entry_points_ptr)
  {
    u8* entry_point =
        m_entry_points_ptr[FastLookupIndexForAddress(ppc_state.pc, ppc_state.feature_flags)];
    if (entry_point)
    {
      return entry_point;
    }
    else
    {
      JitBlock* block = MoveBlockIntoFastCache(ppc_state.pc, ppc_state.feature_flags);

      if (!block)
        return nullptr;

      return block->normalEntry;
    }
  }

  JitBlock* block =
      m_fast_block_map_fallback[FastLookupIndexForAddress(ppc_state.pc, ppc_state.feature_flags)];

  if (!block || block->effectiveAddress != ppc_state.pc ||
      block->feature_flags != ppc_state.feature_flags)
  {
    block = MoveBlockIntoFastCache(ppc_state.pc, ppc_state.feature_flags);
  }

  if (!block)
    return nullptr;

  return block->normalEntry;
}

void JitBaseBlockCache::InvalidateICacheLine(u32 address)
{
  const u32 cache_line_address = address & ~0x1f;
  const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(cache_line_address);
  if (translated.valid)
    InvalidateICacheInternal(translated.address, cache_line_address, 32, false);
}

void JitBaseBlockCache::InvalidateICache(u32 initial_address, u32 initial_length, bool forced)
{
  u32 address = initial_address;
  u32 length = initial_length;
  while (length > 0)
  {
    const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(address);

    const bool address_from_bat = translated.valid && translated.translated && translated.from_bat;
    const int shift = address_from_bat ? PowerPC::BAT_INDEX_SHIFT : PowerPC::HW_PAGE_INDEX_SHIFT;
    const u32 mask = ~((1u << shift) - 1u);
    const u32 first_address = address;
    const u32 last_address = address + (length - 1u);
    if ((first_address & mask) == (last_address & mask))
    {
      if (translated.valid)
        InvalidateICacheInternal(translated.address, address, length, forced);
      return;
    }

    const u32 end_of_page = (first_address + (1u << shift)) & mask;
    const u32 length_this_page = end_of_page - first_address;
    if (translated.valid)
      InvalidateICacheInternal(translated.address, address, length_this_page, forced);
    address = address + length_this_page;
    length = length - length_this_page;
  }
}

void JitBaseBlockCache::InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                                 bool forced)
{
  // Optimization for the case of invalidating a single cache line, which is used by the dcb*
  // instructions. If the valid_block bit for that cacheline is not set, we can safely skip
  // the remaining invalidation logic.
  bool destroy_block = true;
  if (length == 32 && (physical_address & 0x1fu) == 0)
  {
    if (!valid_block.Test(physical_address / 32))
      destroy_block = false;
    else
      valid_block.Clear(physical_address / 32);
  }
  else if (length > 32)
  {
    // Even if we can't check the set for optimization, we still want to remove all fully covered
    // cache lines from the valid_block set so that later calls don't try to invalidate already
    // cleared regions.
    const u32 covered_block_start = (physical_address + 0x1f) / 32;
    const u32 covered_block_end = (physical_address + length) / 32;
    for (u32 i = covered_block_start; i < covered_block_end; ++i)
      valid_block.Clear(i);
  }

  if (destroy_block)
  {
    // destroy JIT blocks
    ErasePhysicalRange(physical_address, length);

    // If the code was actually modified, we need to clear the relevant entries from the
    // FIFO write address cache, so we don't end up with FIFO checks in places they shouldn't
    // be (this can clobber flags, and thus break any optimization that relies on flags
    // being in the right place between instructions).
    if (!forced)
    {
      for (u32 i = address; i < address + length; i += 4)
      {
        m_jit.js.fifoWriteAddresses.erase(i);
        m_jit.js.pairedQuantizeAddresses.erase(i);
        m_jit.js.noSpeculativeConstantsAddresses.erase(i);
      }
    }
  }
}

void JitBaseBlockCache::EraseInMacroBlock(u32 macro_block, u32 address, u32 length)
{
  JitRangeNode* const* const head = jit_range_lists_find(&block_range_map, AddressKey(macro_block));
  JitRangeNode* node = head ? *head : nullptr;
  while (node)
  {
    /* a block is in each macro block once: the next node is another block's */
    JitRangeNode* const next = node->next_in_range;
    JitBlock* const block = node->block;
    if (block->OverlapsPhysicalRange(address, length))
    {
      DestroyBlock(*block);
      for (JitBlock** at = jit_block_lists_find(&block_map, AddressKey(block->physicalAddress));
           at && *at; at = &(*at)->next_at_address)
      {
        if (*at == block)
        {
          *at = block->next_at_address;
          break;
        }
      }
      LeaveMacroBlocks(block);
      ReleaseBlock(block);
    }
    node = next;
  }
}

void JitBaseBlockCache::ErasePhysicalRange(u32 address, u32 length)
{
  // Every macro block which overlaps the given range, and in each, every block that overlaps it.
  const u32 first = address & BLOCK_RANGE_MAP_MASK;
  const u64 end = static_cast<u64>(address) + length;
  const u64 macro_blocks = (end - first + BLOCK_RANGE_SIZE - 1) / BLOCK_RANGE_SIZE;
  if (macro_blocks <= block_range_map.mask + 1)
  {
    u64 i;
    for (i = first; i < end; i += BLOCK_RANGE_SIZE)
      EraseInMacroBlock(static_cast<u32>(i), address, length);
    return;
  }
  /* a range larger than the table: the table's macro blocks in it, from the table. Erasing leaves
   * the table's keys as they are, empty lists included. */
  {
    size_t slot;
    for (slot = 0; slot <= block_range_map.mask; slot++)
    {
      const uintptr_t key = block_range_map.keys[slot];
      if (key != CODE_PTR_MAP_FREE && key >= first && key < end)
        EraseInMacroBlock(static_cast<u32>(key), address, length);
    }
  }
}

u32* JitBaseBlockCache::GetBlockBitSet() const
{
  return valid_block.m_valid_block;
}

void JitBaseBlockCache::WriteDestroyBlock(const JitBlock& block)
{
}

// Block linker
// Make sure to have as many blocks as possible compiled before calling this
// It's O(N), so it's fast :)
// Can be faster by doing a queue for blocks to link up, and only process those
// Should probably be done

void JitBaseBlockCache::LinkBlockExits(JitBlock& block)
{
  for (auto& e : block.linkData)
  {
    if (!e.linkStatus)
    {
      JitBlock* destinationBlock = GetBlockFromStartAddress(e.exitAddress, block.feature_flags);
      if (destinationBlock)
      {
        WriteLinkBlock(e, destinationBlock);
        e.linkStatus = true;
      }
    }
  }
}

void JitBaseBlockCache::LinkBlock(JitBlock& block)
{
  LinkBlockExits(block);
  JitBlock::LinkData* const* const head =
      jit_link_lists_find(&links_to, AddressKey(block.effectiveAddress));

  /* each block with an exit here: once per exit, which links what the first left */
  for (JitBlock::LinkData* e = head ? *head : nullptr; e; e = e->next_to)
  {
    if (block.feature_flags == e->owner->feature_flags)
      LinkBlockExits(*e->owner);
  }
}

void JitBaseBlockCache::UnlinkBlock(const JitBlock& block)
{
  // Unlink all exits of this block.
  for (auto& e : block.linkData)
  {
    WriteLinkBlock(e, nullptr);
  }

  // Unlink all exits of other blocks which points to this block
  JitBlock::LinkData* const* const head =
      jit_link_lists_find(&links_to, AddressKey(block.effectiveAddress));
  for (JitBlock::LinkData* e = head ? *head : nullptr; e; e = e->next_to)
  {
    if (e->owner->feature_flags != block.feature_flags)
      continue;
    WriteLinkBlock(*e, nullptr);
    e->linkStatus = false;
  }
}

void JitBaseBlockCache::DestroyBlock(JitBlock& block)
{
  if (m_entry_points_ptr)
  {
    if (m_entry_points_ptr[block.fast_block_map_index] == block.normalEntry)
    {
      m_entry_points_ptr[block.fast_block_map_index] = nullptr;
    }
  }
  else
  {
    if (m_fast_block_map_fallback[block.fast_block_map_index] == &block)
    {
      m_fast_block_map_fallback[block.fast_block_map_index] = nullptr;
    }
  }

  UnlinkBlock(block);

  // Delete linking addresses
  for (auto& e : block.linkData)
  {
    if (!e.owner)
      continue;
    if (e.prev_to)
    {
      e.prev_to->next_to = e.next_to;
    }
    else
    {
      JitBlock::LinkData** const head = jit_link_lists_find(&links_to, AddressKey(e.exitAddress));
      if (head && *head == &e)
        *head = e.next_to;
    }
    if (e.next_to)
      e.next_to->prev_to = e.prev_to;
    e.owner = nullptr;
    e.next_to = nullptr;
    e.prev_to = nullptr;
  }

  // Raise an signal if we are going to call this block again
  WriteDestroyBlock(block);
}

JitBlock* JitBaseBlockCache::MoveBlockIntoFastCache(u32 addr, CPUEmuFeatureFlags feature_flags)
{
  JitBlock* block = GetBlockFromStartAddress(addr, feature_flags);

  if (!block)
    return nullptr;

  // Drop old fast block map entry
  if (m_entry_points_ptr)
  {
    if (m_entry_points_ptr[block->fast_block_map_index] == block->normalEntry)
    {
      m_entry_points_ptr[block->fast_block_map_index] = nullptr;
    }
  }
  else
  {
    if (m_fast_block_map_fallback[block->fast_block_map_index] == block)
    {
      m_fast_block_map_fallback[block->fast_block_map_index] = nullptr;
    }
  }

  // And create a new one
  size_t index = FastLookupIndexForAddress(addr, feature_flags);
  if (m_entry_points_ptr)
  {
    m_entry_points_arena.EnsureMemoryPageWritable(index * sizeof(u8*));
    m_entry_points_ptr[index] = block->normalEntry;
  }
  else
  {
    m_fast_block_map_fallback[index] = block;
  }
  block->fast_block_map_index = index;

  return block;
}

size_t JitBaseBlockCache::FastLookupIndexForAddress(u32 address, u32 feature_flags)
{
  if (m_entry_points_ptr)
  {
    return (static_cast<size_t>(feature_flags) << 30) | (address >> 2);
  }
  else
  {
    return (address >> 2) & FAST_BLOCK_MAP_FALLBACK_MASK;
  }
}
