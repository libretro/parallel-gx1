// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitCommon/JitCache.h"

#include "Core/PowerPC/CachedInterpreter/CachedInterpreterBlockCache.h"
#ifdef _M_X86_64
#include "Core/PowerPC/Jit64Common/BlockCache.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/JitArm64Cache.h"
#endif

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

/* A page's offset bits */
static const u32 PAGE_OFFSET_MASK = PowerPC::HW_PAGE_MASK;

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
      chunk = static_cast<u8*>(std::malloc(LINK_ARENA_CHUNK));
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

void code_range_list_init(CodeRangeList* list)
{
  list->data = static_cast<CodeRange*>(malloc(CODE_RANGES_START * sizeof(CodeRange)));
  list->count = 0;
  list->cap = list->data ? CODE_RANGES_START : 0;
}

void code_range_list_push(CodeRangeList* list, u8* from, u8* to)
{
  if (list->count == list->cap)
  {
    const size_t cap = list->cap ? list->cap * 2 : CODE_RANGES_START;
    CodeRange* const grown = static_cast<CodeRange*>(realloc(list->data, cap * sizeof(CodeRange)));
    if (!grown)
      abort();
    list->data = grown;
    list->cap = cap;
  }
  list->data[list->count].from = from;
  list->data[list->count].to = to;
  list->count++;
}

/* Where the code is one region: a range next to the last one pushed joins it, so the blocks of
 * a whole cache being cleared, made one after the other, take a few ranges and not one each. */
void code_range_list_push_joined(CodeRangeList* list, u8* from, u8* to)
{
  if (list->count)
  {
    CodeRange* const last = &list->data[list->count - 1];
    if (last->to == from)
    {
      last->to = to;
      return;
    }
    if (last->from == to)
    {
      last->from = from;
      return;
    }
  }
  code_range_list_push(list, from, to);
}

void code_range_list_free(CodeRangeList* list)
{
  free(list->data);
  list->data = nullptr;
  list->count = 0;
  list->cap = 0;
}

bool JitBlock::OverlapsPhysicalRange(u32 address, u32 length) const
{
  return physical_addresses.overlaps(address, address + length);
}

JitBaseBlockCache::JitBaseBlockCache(JitBase& jit) : m_jit{jit}
{
  /* Room from the start for what the heaviest games reach: these tables grow by moving every
   * entry into twice the room, a frame's worth of copying, and keep their room across clears.
   * Their room takes memory only as entries come. */
  jit_link_lists_init(&links_to, 16384);
  jit_block_lists_init(&block_map, 262144);
  // room for the addresses of twice the blocks kept suspended: it never grows (RelistSuspended)
  jit_block_lists_init(&suspended_map, 2 * SUSPENDED_BLOCKS);
  jit_range_lists_init(&block_range_map, 8192);

  /* the blocks, their exits and their macro block nodes the heaviest games reach, past a few
   * thousand frames, in an allocation each that takes memory as they are handed out */
  {
    size_t i;
    m_block_start =
        static_cast<JitBlock*>(calloc(BLOCK_CHUNKS_START * BLOCK_CHUNK, sizeof(JitBlock)));
    m_block_chunks = static_cast<JitBlock**>(malloc(2 * BLOCK_CHUNKS_START * sizeof(JitBlock*)));
    m_link_arena.start = static_cast<u8*>(calloc(LINK_ARENA_START, LINK_ARENA_CHUNK));
    m_link_arena.chunks = static_cast<u8**>(malloc(2 * LINK_ARENA_START * sizeof(u8*)));
    m_range_node_start = static_cast<JitRangeNode*>(
        calloc(RANGE_NODE_CHUNKS_START * RANGE_NODE_CHUNK, sizeof(JitRangeNode)));
    m_range_node_chunks =
        static_cast<JitRangeNode**>(malloc(2 * RANGE_NODE_CHUNKS_START * sizeof(JitRangeNode*)));
    if (!m_block_start || !m_block_chunks || !m_link_arena.start || !m_link_arena.chunks ||
        !m_range_node_start || !m_range_node_chunks)
      abort();
    for (i = 0; i < BLOCK_CHUNKS_START; i++)
      m_block_chunks[i] = m_block_start + i * BLOCK_CHUNK;
    m_num_block_chunks = BLOCK_CHUNKS_START;
    m_block_chunks_cap = 2 * BLOCK_CHUNKS_START;
    for (i = 0; i < LINK_ARENA_START; i++)
      m_link_arena.chunks[i] = m_link_arena.start + i * LINK_ARENA_CHUNK;
    m_link_arena.num_chunks = LINK_ARENA_START;
    m_link_arena.chunks_cap = 2 * LINK_ARENA_START;
    for (i = 0; i < RANGE_NODE_CHUNKS_START; i++)
      m_range_node_chunks[i] = m_range_node_start + i * RANGE_NODE_CHUNK;
    m_num_range_node_chunks = RANGE_NODE_CHUNKS_START;
    m_range_node_chunks_cap = 2 * RANGE_NODE_CHUNKS_START;
  }
}

JitBaseBlockCache::~JitBaseBlockCache()
{
  size_t i;
  for (i = BLOCK_CHUNKS_START; i < m_num_block_chunks; i++)
    free(m_block_chunks[i]);
  free(m_block_start);
  free(m_block_chunks);
  for (i = LINK_ARENA_START; i < m_link_arena.num_chunks; i++)
    free(m_link_arena.chunks[i]);
  free(m_link_arena.start);
  free(m_link_arena.chunks);
  for (i = RANGE_NODE_CHUNKS_START; i < m_num_range_node_chunks; i++)
    free(m_range_node_chunks[i]);
  free(m_range_node_start);
  free(m_range_node_chunks);
  jit_range_lists_free(&block_range_map);
  jit_block_lists_free(&block_map);
  jit_block_lists_free(&suspended_map);
  jit_link_lists_free(&links_to);
}

JitBlock* JitBaseBlockCache::TakeBlock()
{
  JitBlock* block = m_free_blocks;
  if (block)
  {
    m_free_blocks = block->next_at_address;
  }
  else
  {
    /* a new one: all zero, as a JitBlock starts */
    if (m_blocks_made == m_num_block_chunks * BLOCK_CHUNK)
    {
      JitBlock* chunk;
      if (m_num_block_chunks == m_block_chunks_cap)
      {
        const size_t cap = m_block_chunks_cap * 2;
        JitBlock** const chunks =
            static_cast<JitBlock**>(realloc(m_block_chunks, cap * sizeof(*chunks)));
        if (!chunks)
          abort();
        m_block_chunks = chunks;
        m_block_chunks_cap = cap;
      }
      chunk = static_cast<JitBlock*>(calloc(BLOCK_CHUNK, sizeof(JitBlock)));
      if (!chunk)
        abort();
      m_block_chunks[m_num_block_chunks++] = chunk;
    }
    block = BlockAt(m_blocks_made++);
  }
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
  node = m_free_range_nodes;
  if (node)
  {
    m_free_range_nodes = node->next_of_block;
  }
  else
  {
    if (m_range_nodes_made == m_num_range_node_chunks * RANGE_NODE_CHUNK)
    {
      JitRangeNode* chunk;
      if (m_num_range_node_chunks == m_range_node_chunks_cap)
      {
        const size_t cap = m_range_node_chunks_cap * 2;
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
    }
    node = &m_range_node_chunks[m_range_nodes_made / RANGE_NODE_CHUNK]
                               [m_range_nodes_made % RANGE_NODE_CHUNK];
    m_range_nodes_made++;
  }

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
  /* the blocks handed out: the others are as they started */
  size_t i;
  m_clearing = true;
  for (i = 0; i < m_blocks_made; i++)
  {
    if (BlockAt(i)->in_use)
      DestroyBlock(*BlockAt(i));
  }
  m_clearing = false;
  for (i = 0; i < m_blocks_made; i++)
  {
    JitBlock* const block = BlockAt(i);
    if (block->in_use)
    {
      LeaveMacroBlocks(block);
      ReleaseBlock(block);
    }
  }
  /* every block's exits, the free ones' included, gave their room back to the arena */
  for (i = 0; i < m_blocks_made; i++)
  {
    JitBlock::LinkDataList& exits = BlockAt(i)->linkData;
    exits.data = nullptr;
    exits.count = 0;
    exits.cap = 0;
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
  b.second_page_ea = 0;
  b.second_page_pa = 0;
  b.two_pages = false;
  b.suspended = false;
  b.entry_closed = false;
  b.incoming_unlinked = false;
  b.next_suspended = nullptr;
  b.prev_suspended = nullptr;
  b.newer_suspended = nullptr;
  b.older_suspended = nullptr;
  return &b;
}

/* Code from two pages, the effective one after the first and its physical page not the first's,
 * every range in one of them: the block keeps the second page, to be taken back with its code
 * wherever each page is in RAM then */
void JitBaseBlockCache::FindSecondPage(JitBlock& block, const PPCAnalyst::CodeBlock& code_block,
                                       const PPCAnalyst::CodeBuffer& code_buffer)
{
  const u32 first_ea = block.effectiveAddress & ~PAGE_OFFSET_MASK;
  const u32 first_pa = block.physicalAddress & ~PAGE_OFFSET_MASK;
  const u32 second_ea = first_ea + PAGE_OFFSET_MASK + 1;
  u32 second_pa, i;
  bool other = false;

  for (i = 0; i < code_block.m_num_instructions; i++)
  {
    const u32 page = code_buffer[i].address & ~PAGE_OFFSET_MASK;
    if (page == second_ea)
      other = true;
    else if (page != first_ea)
      return;
  }
  if (!other || !TranslateCodeAddress(second_ea, block.feature_flags, &second_pa))
    return;
  second_pa &= ~PAGE_OFFSET_MASK;
  if (second_pa == first_pa)
    return;
  for (const SmallRange& range : block.physical_addresses)
  {
    u32 from = range.from;
    while (from < range.to)
    {
      const u32 page_end = (from | PAGE_OFFSET_MASK) + 1;
      if ((from & ~PAGE_OFFSET_MASK) != first_pa && (from & ~PAGE_OFFSET_MASK) != second_pa)
        return;
      if (!page_end)
        break;
      from = page_end;
    }
  }
  block.second_page_ea = second_ea;
  block.second_page_pa = second_pa;
  block.two_pages = true;
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
  FindSecondPage(block, code_block, code_buffer);
  if (!HashCode(block, block.physicalAddress & ~PAGE_OFFSET_MASK, block.second_page_pa,
                &block.code_hash))
    block.code_hash = 0;

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

/* The physical address the code at the effective address is at, false when it does not translate */
bool JitBaseBlockCache::TranslateCodeAddress(u32 addr, CPUEmuFeatureFlags feature_flags,
                                             u32* physical_address)
{
  if (feature_flags & FEATURE_FLAG_MSR_IR)
  {
    const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(addr);
    if (!translated.valid)
      return false;
    *physical_address = translated.address;
    return true;
  }
  *physical_address = addr;
  return true;
}

/* The block that runs at the effective address, its code at the physical address */
JitBlock* JitBaseBlockCache::FindBlock(u32 addr, u32 physical_address,
                                       CPUEmuFeatureFlags feature_flags)
{
  JitBlock* const* const head = jit_block_lists_find(&block_map, AddressKey(physical_address));
  for (JitBlock* b = head ? *head : nullptr; b; b = b->next_at_address)
  {
    if (b->effectiveAddress == addr && b->feature_flags == feature_flags && !b->suspended)
      return b;
  }
  return nullptr;
}

JitBlock* JitBaseBlockCache::GetBlockFromStartAddress(u32 addr, CPUEmuFeatureFlags feature_flags)
{
  u32 physical_address;
  if (!TranslateCodeAddress(addr, feature_flags, &physical_address))
    return nullptr;
  return FindBlock(addr, physical_address, feature_flags);
}

/* Out of the lookups and the lists by physical address, its code kept, first in the list of those
 * suspended at its effective address; false, and nothing done, without room for that list */
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
  /* closed, a block keeps its links and is linked to while suspended: taken back, it opens again
   * and only links what was left unlinked */
  if (!WriteCloseEntry(block))
  {
    UnlinkBlock(block);
    for (auto& e : block.linkData)
      e.linkStatus = false;
    block.incoming_unlinked = true;
  }

  /* the code where it was is another's now: it is placed again wherever its code is when it is
   * taken back */
  LeavePhysicalLists(&block);

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

/* Out of the cache's lists by physical address, if it is in them */
void JitBaseBlockCache::LeavePhysicalLists(JitBlock* block)
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
  LeaveMacroBlocks(block);
}

/* Destroyed and out of every list, back in the pool */
void JitBaseBlockCache::EraseBlock(JitBlock* block)
{
  LeavePhysicalLists(block);
  DestroyBlock(*block);
  ReleaseBlock(block);
}

/* Back in the cache's lists by physical address, with its pages at first_page and second_page,
 * where its code is in RAM now. The code's hash is keyed by offset within its pages, so
 * code_hash, found there, stays as it is. */
void JitBaseBlockCache::PlaceBlock(JitBlock* block, u32 first_page, u32 second_page)
{
  SmallRangeSet moved;
  JitBlock** at;

  for (const SmallRange& range : block->physical_addresses)
  {
    u32 from = range.from;
    while (from < range.to)
    {
      const u32 page_end = (from | PAGE_OFFSET_MASK) + 1;
      const u32 to = page_end && page_end < range.to ? page_end : range.to;
      u32 mapped, key;
      if (MapCodeAddress(*block, from, first_page, second_page, &mapped, &key))
        moved.insert(mapped, mapped + (to - from));
      from = to;
    }
  }
  block->physical_addresses = moved;
  block->physicalAddress = first_page | (block->physicalAddress & PAGE_OFFSET_MASK);
  if (block->two_pages)
    block->second_page_pa = second_page;

  block->next_at_address = nullptr;
  at = jit_block_lists_get(&block_map, AddressKey(block->physicalAddress));
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
JitBlock* JitBaseBlockCache::ResumeSuspendedBlock(u32 addr, u32 physical_address,
                                                  CPUEmuFeatureFlags feature_flags)
{
  u32 others = 0, second_ea = 0, second_pa = 0;
  bool second_valid = false;
  JitBlock* const* const head = jit_block_lists_find(&suspended_map, AddressKey(addr));
  JitBlock* b;
  if (!head || !*head)
    return nullptr;

  for (b = *head; b; b = b->next_suspended)
  {
    const u32 first_page = physical_address & ~PAGE_OFFSET_MASK;
    u32 second_page = b->second_page_pa;
    u64 hash;
    if (b->feature_flags != feature_flags)
      continue;
    if (b->two_pages)
    {
      if (b->second_page_ea != second_ea)
      {
        second_ea = b->second_page_ea;
        second_valid = TranslateCodeAddress(second_ea, feature_flags, &second_pa);
        second_pa &= ~PAGE_OFFSET_MASK;
      }
      second_page = second_pa;
    }
    if ((!b->two_pages || second_valid) && b->code_hash &&
        HashCode(*b, first_page, second_page, &hash) && hash == b->code_hash)
    {
      LeaveSuspended(*b);
      if (b->entry_closed)
        WriteOpenEntry(*b);
      PlaceBlock(b, first_page, second_page);
      for (const SmallRange& range : b->physical_addresses)
      {
        for (u32 i = range.from & ~31u; i < range.to; i += 32)
          valid_block.Set(i / 32);
      }
      if (m_jit.jo.enableBlocklink)
      {
        LinkBlockExits(*b);
        if (b->incoming_unlinked)
          LinkIncoming(*b);
      }
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
/* Where a physical address of the block's code is with its pages at first_page and second_page,
 * and a key for it the same wherever they are; false for code outside its pages when they moved */
bool JitBaseBlockCache::MapCodeAddress(const JitBlock& block, u32 address, u32 first_page,
                                       u32 second_page, u32* mapped, u32* key) const
{
  const u32 page = address & ~PAGE_OFFSET_MASK;
  const u32 old_first = block.physicalAddress & ~PAGE_OFFSET_MASK;
  if (page == old_first)
  {
    *mapped = first_page | (address & PAGE_OFFSET_MASK);
    *key = address & PAGE_OFFSET_MASK;
    return true;
  }
  if (block.two_pages && page == block.second_page_pa)
  {
    *mapped = second_page | (address & PAGE_OFFSET_MASK);
    *key = (PAGE_OFFSET_MASK + 1) | (address & PAGE_OFFSET_MASK);
    return true;
  }
  if (first_page != old_first)
    return false;
  *mapped = address;
  *key = address;
  return true;
}

/* A hash of the block's code with its pages at first_page and second_page; false when part of it
 * is not in RAM there */
bool JitBaseBlockCache::HashCode(const JitBlock& block, u32 first_page, u32 second_page,
                                 u64* hash_out) const
{
  auto& memory = m_jit.m_system.GetMemory();
  u64 hash = 0x9E3779B97F4A7C15ull;
  for (const SmallRange& range : block.physical_addresses)
  {
    u32 from = range.from;
    while (from < range.to)
    {
      const u32 page_end = (from | PAGE_OFFSET_MASK) + 1;
      const u32 to = page_end && page_end < range.to ? page_end : range.to;
      const u32 size = to - from;
      u32 mapped, key, address, i;
      const u8* code;
      if (!MapCodeAddress(block, from, first_page, second_page, &mapped, &key))
        return false;
      address = mapped & 0x3FFFFFFF;
      if (address < memory.GetRamSizeReal() && size <= memory.GetRamSizeReal() - address)
        code = memory.GetRAM() + address;
      else if (memory.GetEXRAM() && (address >> 28) == 0x1 &&
               (address & 0x0fffffff) < memory.GetExRamSizeReal() &&
               size <= memory.GetExRamSizeReal() - (address & 0x0fffffff))
        code = memory.GetEXRAM() + (address & memory.GetExRamMask());
      else
        return false;
      hash = (hash ^ key) * 0xFF51AFD7ED558CCDull;
      for (i = 0; i + 4 <= size; i += 4)
      {
        u32 word;
        std::memcpy(&word, code + i, sizeof(word));
        hash = (hash ^ word) * 0xC4CEB9FE1A85EC53ull;
        hash ^= hash >> 31;
      }
      from = to;
    }
  }
  *hash_out = hash ? hash : 1;
  return true;
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
        !(suspend && block->code_hash && SuspendBlock(*block)))
    {
      DestroyBlock(*block);
      LeavePhysicalLists(block);
      ReleaseBlock(block);
    }
    node = next;
  }
}

void JitBaseBlockCache::ErasePhysicalRange(u32 address, u32 length, bool suspend)
{
  /* what is forced goes, the suspended blocks whose code was there included */
  if (!suspend)
  {
    JitBlock* b = m_oldest_suspended;
    while (b)
    {
      JitBlock* const newer = b->newer_suspended;
      if (b->OverlapsPhysicalRange(address, length))
        EraseBlock(b);
      b = newer;
    }
  }

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
      uintptr_t key;
      if (jit_range_lists_slot_key(&block_range_map, slot, &key) && key >= first && key < end)
        EraseInMacroBlock(static_cast<u32>(key), address, length, suspend);
    }
  }
}

u32* JitBaseBlockCache::GetBlockBitSet() const
{
  return valid_block.m_valid_block;
}

#ifdef _M_X86_64
#define BLOCK_CACHE_JIT64(call)                                                                    \
  case CPUCoreKind::Jit64:                                                                         \
    static_cast<JitBlockCache*>(this)->call;                                                       \
    break;
#else
#define BLOCK_CACHE_JIT64(call)
#endif
#ifdef _M_ARM_64
#define BLOCK_CACHE_JITARM64(call)                                                                 \
  case CPUCoreKind::JitArm64:                                                                      \
    static_cast<JitArm64BlockCache*>(this)->call;                                                  \
    break;
#else
#define BLOCK_CACHE_JITARM64(call)
#endif

/* The call made on the block cache of the JIT this is */
#define BLOCK_CACHE_CALL(call)                                                                     \
  switch (m_jit.cpu_core_kind)                                                                     \
  {                                                                                                \
  case CPUCoreKind::CachedInterpreter:                                                             \
    static_cast<CachedInterpreterBlockCache*>(this)->call;                                         \
    break;                                                                                         \
    BLOCK_CACHE_JIT64(call)                                                                        \
    BLOCK_CACHE_JITARM64(call)                                                                     \
  default:                                                                                         \
    break;                                                                                         \
  }

void JitBaseBlockCache::DestroyBlock(JitBlock& block)
{
  BLOCK_CACHE_CALL(DestroyBlock(block))
}

void JitBaseBlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  BLOCK_CACHE_CALL(WriteLinkBlock(source, dest))
}

void JitBaseBlockCache::WriteDestroyBlock(const JitBlock& block)
{
  BLOCK_CACHE_CALL(WriteDestroyBlock(block))
}

/* The block's entry sent to the dispatcher, the code there kept; false, nothing done, for a JIT
 * that cannot */
bool JitBaseBlockCache::WriteCloseEntry(JitBlock& block)
{
#ifdef _M_X86_64
  if (m_jit.cpu_core_kind == CPUCoreKind::Jit64)
  {
    static_cast<JitBlockCache*>(this)->WriteCloseEntry(block);
    block.entry_closed = true;
    return true;
  }
#endif
  return false;
}

void JitBaseBlockCache::WriteOpenEntry(JitBlock& block)
{
#ifdef _M_X86_64
  if (m_jit.cpu_core_kind == CPUCoreKind::Jit64)
    static_cast<JitBlockCache*>(this)->WriteOpenEntry(block);
#endif
  block.entry_closed = false;
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
      else
      {
        NoteUnlinkedExits(e.exitAddress, block.feature_flags);
      }
    }
  }
}

void JitBaseBlockCache::LinkBlock(JitBlock& block)
{
  LinkBlockExits(block);
  LinkIncoming(block);
}

/* An exit to the effective address left unlinked: the blocks suspended there link those coming in
 * when they are taken back */
void JitBaseBlockCache::NoteUnlinkedExits(u32 address, CPUEmuFeatureFlags feature_flags)
{
  JitBlock* const* const head = jit_block_lists_find(&suspended_map, AddressKey(address));
  JitBlock* b;
  for (b = head ? *head : nullptr; b; b = b->next_suspended)
  {
    if (b->feature_flags == feature_flags)
      b->incoming_unlinked = true;
  }
}

void JitBaseBlockCache::LinkIncoming(JitBlock& block)
{
  block.incoming_unlinked = false;
  JitBlock::LinkData* const* const head =
      jit_link_lists_find(&links_to, AddressKey(block.effectiveAddress));

  /* each unlinked exit here of a block that can run goes to this block; its owner's other exits
   * were linked when it or their blocks came */
  for (JitBlock::LinkData* e = head ? *head : nullptr; e; e = e->next_to)
  {
    if (e->linkStatus || e->owner->suspended || block.feature_flags != e->owner->feature_flags)
      continue;
    WriteLinkBlock(*e, &block);
    e->linkStatus = true;
  }
}

/* true when it unlinked an exit to it */
bool JitBaseBlockCache::UnlinkBlock(const JitBlock& block)
{
  // Unlink all exits of this block.
  for (auto& e : block.linkData)
  {
    WriteLinkBlock(e, nullptr);
  }

  // Unlink all exits of other blocks which points to this block
  bool unlinked = false;
  JitBlock::LinkData* const* const head =
      jit_link_lists_find(&links_to, AddressKey(block.effectiveAddress));
  for (JitBlock::LinkData* e = head ? *head : nullptr; e; e = e->next_to)
  {
    if (e->owner->feature_flags != block.feature_flags)
      continue;
    WriteLinkBlock(*e, nullptr);
    e->linkStatus = false;
    unlinked = true;
  }
  return unlinked;
}

void JitBaseBlockCache::DestroyBlockCommon(JitBlock& block)
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

  /* some were linked to another block there, which links them again when it is taken back; a
   * clear destroys them all */
  if (UnlinkBlock(block) && !m_clearing)
  {
    JitBlock* const other = GetBlockFromStartAddress(block.effectiveAddress, block.feature_flags);
    if (other && other != &block)
      other->incoming_unlinked = true;
    NoteUnlinkedExits(block.effectiveAddress, block.feature_flags);
  }

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
  u32 physical_address;
  JitBlock* block;
  if (!TranslateCodeAddress(addr, feature_flags, &physical_address))
    return nullptr;
  block = FindBlock(addr, physical_address, feature_flags);
  if (!block)
    block = ResumeSuspendedBlock(addr, physical_address, feature_flags);

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
