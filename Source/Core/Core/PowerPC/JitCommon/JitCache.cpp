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
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace Gen;

/* An address as the key of a code address map */
static const void* AddressKey(u32 address)
{
  return reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
}

/* @bytes from the arena, 16-aligned; a new chunk once the one handed out from is used up */
static void* LinkArenaAlloc(LinkDataArena* arena, size_t bytes)
{
  bytes = (bytes + 15) & ~static_cast<size_t>(15);
  if (bytes > LINK_ARENA_CHUNK)
    abort();
  if (!arena->num_chunks || arena->used + bytes > LINK_ARENA_CHUNK)
  {
    if (arena->num_chunks)
      arena->current++;
    arena->used = 0;
    if (arena->current == arena->num_chunks)
    {
      u8* chunk;
      if (arena->num_chunks == arena->chunks_cap)
      {
        const size_t cap = arena->chunks_cap ? arena->chunks_cap * 2 : 8;
        u8** const chunks = static_cast<u8**>(realloc(arena->chunks, cap * sizeof(*chunks)));
        if (!chunks)
          abort();
        arena->chunks = chunks;
        arena->chunks_cap = cap;
      }
      chunk = static_cast<u8*>(malloc(LINK_ARENA_CHUNK));
      if (!chunk)
        abort();
      arena->chunks[arena->num_chunks++] = chunk;
    }
  }
  {
    void* const region = arena->chunks[arena->current] + arena->used;
    arena->used += bytes;
    return region;
  }
}

/* @region, @old_bytes long, made @new_bytes long where it is: when it is the last handed out and
 * its chunk has the room */
static bool LinkArenaExtend(LinkDataArena* arena, void* region, size_t old_bytes, size_t new_bytes)
{
  u8* const chunk = arena->num_chunks ? arena->chunks[arena->current] : nullptr;
  old_bytes = (old_bytes + 15) & ~static_cast<size_t>(15);
  new_bytes = (new_bytes + 15) & ~static_cast<size_t>(15);
  if (!chunk || static_cast<u8*>(region) + old_bytes != chunk + arena->used ||
      arena->used - old_bytes + new_bytes > LINK_ARENA_CHUNK)
    return false;
  arena->used = arena->used - old_bytes + new_bytes;
  return true;
}

void JitBlock::LinkDataList::push_back(const LinkData& exit)
{
  if (count == cap)
  {
    const u32 new_cap = cap ? cap * 2 : 8;
    if (!data || !LinkArenaExtend(arena, data, cap * sizeof(LinkData), new_cap * sizeof(LinkData)))
    {
      LinkData* const moved =
          static_cast<LinkData*>(LinkArenaAlloc(arena, new_cap * sizeof(LinkData)));
      u32 i;
      for (i = 0; i < count; i++)
        moved[i] = data[i];
      data = moved;
    }
    cap = new_cap;
  }
  data[count++] = exit;
}

bool JitBlock::OverlapsPhysicalRange(u32 address, u32 length) const
{
  return physical_addresses.overlaps(address, address + length);
}

JitBaseBlockCache::JitBaseBlockCache(JitBase& jit) : m_jit{jit}
{
  jit_link_lists_init(&links_to, 4096);
  jit_block_lists_init(&block_map, 8192);
  // room for the addresses of twice the blocks kept suspended: it never grows (RelistSuspended)
  jit_block_lists_init(&suspended_map, 2 * SUSPENDED_BLOCKS);
  jit_range_lists_init(&block_range_map, 8192);
}

JitBaseBlockCache::~JitBaseBlockCache()
{
  size_t i;
  for (i = 0; i < m_num_block_chunks; i++)
    delete[] m_block_chunks[i];
  free(m_block_chunks);
  for (i = 0; i < m_link_arena.num_chunks; i++)
    free(m_link_arena.chunks[i]);
  free(m_link_arena.chunks);
  for (i = 0; i < m_num_range_node_chunks; i++)
    free(m_range_node_chunks[i]);
  free(m_range_node_chunks);
  jit_range_lists_free(&block_range_map);
  jit_block_lists_free(&block_map);
  jit_block_lists_free(&suspended_map);
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
  Core::DisplayMessage("Clearing code cache.");
#endif
  address_set_clear(&m_jit.js.fifoWriteAddresses);
  address_set_clear(&m_jit.js.pairedQuantizeAddresses);
  address_set_clear(&m_jit.js.noSpeculativeConstantsAddresses);
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
  /* every block's exits, the free ones' included, gave their room back to the arena */
  for (chunk = 0; chunk < m_num_block_chunks; chunk++)
  {
    for (i = 0; i < BLOCK_CHUNK; i++)
    {
      JitBlock::LinkDataList& exits = m_block_chunks[chunk][i].linkData;
      exits.data = nullptr;
      exits.count = 0;
      exits.cap = 0;
    }
  }
  m_link_arena.current = 0;
  m_link_arena.used = 0;
  jit_block_lists_clear(&block_map);
  jit_block_lists_clear(&suspended_map);
  m_oldest_suspended = nullptr;
  m_newest_suspended = nullptr;
  m_num_suspended = 0;
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
  b.linkData.arena = &m_link_arena;
  b.fast_block_map_index = 0;
  b.code_hash = 0;
  b.suspended = false;
  b.next_suspended = nullptr;
  b.prev_suspended = nullptr;
  b.newer_suspended = nullptr;
  b.older_suspended = nullptr;
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
  block.code_hash = HashCode(block, block.physicalAddress);

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
    if (b->effectiveAddress == addr && b->feature_flags == feature_flags && !b->suspended)
      return b;
  }

  return nullptr;
}

/* Out of the lookups and unlinked, its code and its place in the cache's lists kept, first in
 * the list of those suspended at its effective address; false, and nothing done, without room
 * for that list */
bool JitBaseBlockCache::SuspendBlock(JitBlock& block)
{
  if (suspended_map.count >= 2 * SUSPENDED_BLOCKS && suspended_map.count >= 2 * m_num_suspended)
    RelistSuspended();
  JitBlock** const head = jit_block_lists_get(&suspended_map, AddressKey(block.effectiveAddress));
  if (!head)
    return false;

  if (m_entry_points_ptr)
  {
    if (m_entry_points_ptr[block.fast_block_map_index] == block.normalEntry)
      m_entry_points_ptr[block.fast_block_map_index] = nullptr;
  }
  else if (m_fast_block_map_fallback[block.fast_block_map_index] == &block)
  {
    m_fast_block_map_fallback[block.fast_block_map_index] = nullptr;
  }
  UnlinkBlock(block);
  for (auto& e : block.linkData)
    e.linkStatus = false;

  block.suspended = true;
  block.prev_suspended = nullptr;
  block.next_suspended = *head;
  if (*head)
    (*head)->prev_suspended = &block;
  *head = &block;

  block.newer_suspended = nullptr;
  block.older_suspended = m_newest_suspended;
  if (m_newest_suspended)
    m_newest_suspended->newer_suspended = &block;
  else
    m_oldest_suspended = &block;
  m_newest_suspended = &block;
  m_num_suspended++;
  return true;
}

/* The map of suspended blocks by address keeps an address whose list has emptied: once those are
 * half of it, it is made again from the blocks suspended now, oldest first so each address's list
 * is newest first again */
void JitBaseBlockCache::RelistSuspended()
{
  jit_block_lists_clear(&suspended_map);
  for (JitBlock* b = m_oldest_suspended; b; b = b->newer_suspended)
  {
    JitBlock** const head = jit_block_lists_get(&suspended_map, AddressKey(b->effectiveAddress));
    b->prev_suspended = nullptr;
    b->next_suspended = nullptr;
    if (!head)
      continue;
    b->next_suspended = *head;
    if (*head)
      (*head)->prev_suspended = b;
    *head = b;
  }
}

void JitBaseBlockCache::LeaveSuspended(JitBlock& block)
{
  if (!block.suspended)
    return;
  if (block.prev_suspended)
  {
    block.prev_suspended->next_suspended = block.next_suspended;
  }
  else
  {
    JitBlock** const head =
        jit_block_lists_find(&suspended_map, AddressKey(block.effectiveAddress));
    if (head && *head == &block)
      *head = block.next_suspended;
  }
  if (block.next_suspended)
    block.next_suspended->prev_suspended = block.prev_suspended;
  block.next_suspended = nullptr;
  block.prev_suspended = nullptr;

  if (block.newer_suspended)
    block.newer_suspended->older_suspended = block.older_suspended;
  else
    m_newest_suspended = block.older_suspended;
  if (block.older_suspended)
    block.older_suspended->newer_suspended = block.newer_suspended;
  else
    m_oldest_suspended = block.newer_suspended;
  block.newer_suspended = nullptr;
  block.older_suspended = nullptr;
  m_num_suspended--;
  block.suspended = false;
}

/* Destroyed and out of every list, back in the pool */
void JitBaseBlockCache::EraseBlock(JitBlock* block)
{
  for (JitBlock** at = jit_block_lists_find(&block_map, AddressKey(block->physicalAddress));
       at && *at; at = &(*at)->next_at_address)
  {
    if (*at == block)
    {
      *at = block->next_at_address;
      break;
    }
  }
  DestroyBlock(*block);
  LeaveMacroBlocks(block);
  ReleaseBlock(block);
}

/* Its page is elsewhere in RAM now: the cache's lists by physical address have it there */
void JitBaseBlockCache::MoveBlock(JitBlock* block, u32 physical_address)
{
  const u32 delta = physical_address - block->physicalAddress;
  JitBlock** at;
  u32 r;

  for (at = jit_block_lists_find(&block_map, AddressKey(block->physicalAddress)); at && *at;
       at = &(*at)->next_at_address)
  {
    if (*at == block)
    {
      *at = block->next_at_address;
      break;
    }
  }
  LeaveMacroBlocks(block);

  block->physicalAddress = physical_address;
  for (r = 0; r < block->physical_addresses.count; r++)
  {
    block->physical_addresses.ranges[r].from += delta;
    block->physical_addresses.ranges[r].to += delta;
  }

  block->next_at_address = nullptr;
  at = jit_block_lists_get(&block_map, AddressKey(physical_address));
  while (*at)
    at = &(*at)->next_at_address;
  *at = block;
  for (const SmallRange& range : block->physical_addresses)
  {
    for (u32 i = range.from & BLOCK_RANGE_MAP_MASK; i < range.to; i += BLOCK_RANGE_SIZE)
      AddToMacroBlock(block, i);
  }
}

/* The block suspended at the effective address whose code is in RAM where the address is now,
 * taken back, its page moved if the game paged it in elsewhere. Only a block all in one page of
 * its own can move. A game can load one of a few pieces of code into the same place in turn, so
 * the others stay suspended for when theirs is back, the newest few: older ones are destroyed, as
 * the invalidation would have done. */
JitBlock* JitBaseBlockCache::ResumeSuspendedBlock(u32 addr, CPUEmuFeatureFlags feature_flags)
{
  u32 physical_address = addr;
  u32 others = 0;
  JitBlock* const* const head = jit_block_lists_find(&suspended_map, AddressKey(addr));
  JitBlock* b;
  if (!head || !*head)
    return nullptr;
  if (feature_flags & FEATURE_FLAG_MSR_IR)
  {
    auto translated = m_jit.m_mmu.JitCache_TranslateAddress(addr);
    if (!translated.valid)
      return nullptr;
    physical_address = translated.address;
  }

  for (b = *head; b; b = b->next_suspended)
  {
    bool in_page = true;
    if (b->feature_flags != feature_flags)
      continue;
    if (b->physicalAddress != physical_address)
    {
      for (const SmallRange& range : b->physical_addresses)
      {
        if (((range.from ^ b->physicalAddress) | ((range.to - 1) ^ b->physicalAddress)) &
            ~static_cast<u32>(PowerPC::HW_PAGE_MASK))
          in_page = false;
      }
    }
    if (in_page && HashCode(*b, physical_address) == b->code_hash)
    {
      LeaveSuspended(*b);
      if (b->physicalAddress != physical_address)
        MoveBlock(b, physical_address);
      for (const SmallRange& range : b->physical_addresses)
      {
        for (u32 i = range.from & ~31u; i < range.to; i += 32)
          valid_block.Set(i / 32);
      }
      if (m_jit.jo.enableBlocklink)
        LinkBlock(*b);
      return b;
    }
    others++;
  }

  /* the oldest are last: room for the one about to be compiled */
  while (others >= SUSPENDED_VERSIONS)
  {
    JitBlock* oldest = nullptr;
    for (b = *head; b; b = b->next_suspended)
    {
      if (b->feature_flags == feature_flags)
        oldest = b;
    }
    if (!oldest)
      break;
    EraseBlock(oldest);
    others--;
  }
  return nullptr;
}

bool JitBaseBlockCache::ResumeBlock(u32 em_address, CPUEmuFeatureFlags feature_flags)
{
  // here, before a compile, nothing is walking the cache's lists: the oldest suspended past the
  // limit go
  while (m_num_suspended > SUSPENDED_BLOCKS)
    EraseBlock(m_oldest_suspended);
  return MoveBlockIntoFastCache(em_address, feature_flags) != nullptr;
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
    // destroy JIT blocks; what the game invalidates is only suspended, and taken back when its
    // code is in RAM again: a game that pages the same code back in, or loads one of a few pieces
    // of code into the same place in turn, gets the blocks it had, not a recompile of all of it.
    // What is forced is destroyed.
    ErasePhysicalRange(physical_address, length, !forced);

    // If the code was actually modified, we need to clear the relevant entries from the
    // FIFO write address cache, so we don't end up with FIFO checks in places they shouldn't
    // be (this can clobber flags, and thus break any optimization that relies on flags
    // being in the right place between instructions).
    if (!forced)
    {
      for (u32 i = address; i < address + length; i += 4)
      {
        address_set_erase(&m_jit.js.fifoWriteAddresses, i);
        address_set_erase(&m_jit.js.pairedQuantizeAddresses, i);
        address_set_erase(&m_jit.js.noSpeculativeConstantsAddresses, i);
      }
    }
  }
}

/* The code of the block as if its physicalAddress were @physical_address, word by word, its
 * ranges taken from there; 0 when any of it is not in RAM */
u64 JitBaseBlockCache::HashCode(const JitBlock& block, u32 physical_address) const
{
  auto& memory = m_jit.m_system.GetMemory();
  u64 hash = 0x9E3779B97F4A7C15ull;
  for (const SmallRange& range : block.physical_addresses)
  {
    const u32 offset = range.from - block.physicalAddress;
    const u32 address = (physical_address + offset) & 0x3FFFFFFF;
    const u32 size = range.to - range.from;
    const u8* code;
    u32 i;
    if (address < memory.GetRamSizeReal() && size <= memory.GetRamSizeReal() - address)
      code = memory.GetRAM() + address;
    else if (memory.GetEXRAM() && (address >> 28) == 0x1 &&
             (address & 0x0fffffff) < memory.GetExRamSizeReal() &&
             size <= memory.GetExRamSizeReal() - (address & 0x0fffffff))
      code = memory.GetEXRAM() + (address & memory.GetExRamMask());
    else
      return 0;
    hash = (hash ^ offset) * 0xFF51AFD7ED558CCDull;
    for (i = 0; i + 4 <= size; i += 4)
    {
      u32 word;
      std::memcpy(&word, code + i, sizeof(word));
      hash = (hash ^ word) * 0xC4CEB9FE1A85EC53ull;
      hash ^= hash >> 31;
    }
  }
  return hash ? hash : 1;
}

void JitBaseBlockCache::EraseInMacroBlock(u32 macro_block, u32 address, u32 length, bool suspend)
{
  JitRangeNode* const* const head = jit_range_lists_find(&block_range_map, AddressKey(macro_block));
  JitRangeNode* node = head ? *head : nullptr;
  while (node)
  {
    /* a block is in each macro block once: the next node is another block's */
    JitRangeNode* const next = node->next_in_range;
    JitBlock* const block = node->block;
    // one suspended instead is kept for when its code is in RAM again
    if (block->OverlapsPhysicalRange(address, length) &&
        !(suspend && block->code_hash && (block->suspended || SuspendBlock(*block))))
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

void JitBaseBlockCache::ErasePhysicalRange(u32 address, u32 length, bool suspend)
{
  // Every macro block which overlaps the given range, and in each, every block that overlaps it.
  const u32 first = address & BLOCK_RANGE_MAP_MASK;
  const u64 end = static_cast<u64>(address) + length;
  const u64 macro_blocks = (end - first + BLOCK_RANGE_SIZE - 1) / BLOCK_RANGE_SIZE;
  if (macro_blocks <= block_range_map.mask + 1)
  {
    u64 i;
    for (i = first; i < end; i += BLOCK_RANGE_SIZE)
      EraseInMacroBlock(static_cast<u32>(i), address, length, suspend);
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
        EraseInMacroBlock(static_cast<u32>(key), address, length, suspend);
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
  LeaveSuspended(block);
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
    block = ResumeSuspendedBlock(addr, feature_flags);

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
