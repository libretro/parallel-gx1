// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/PPCCache.h"

#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/MemoryUtil.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace PowerPC
{
namespace
{
constexpr u32 s_plru_mask[8] = {
    11, 11, 19, 19, 37, 37, 69, 69,
};
constexpr u32 s_plru_value[8] = {
    11, 3, 17, 1, 36, 4, 64, 0,
};

// The first way not valid, for each valid mask but the full one
constexpr u8 s_way_from_valid[255] = {
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 7,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};

// The way the tree of PLRU bits points at, for each PLRU state
constexpr u8 s_way_from_plru[128] = {
    0, 4, 2, 4, 0, 6, 2, 6, 1, 4, 2, 4, 1, 6, 2, 6, 0, 4, 3, 4, 0, 6, 3, 6, 1, 4, 3, 4, 1, 6, 3, 6,
    0, 5, 2, 5, 0, 6, 2, 6, 1, 5, 2, 5, 1, 6, 2, 6, 0, 5, 3, 5, 0, 6, 3, 6, 1, 5, 3, 5, 1, 6, 3, 6,
    0, 4, 2, 4, 0, 7, 2, 7, 1, 4, 2, 4, 1, 7, 2, 7, 0, 4, 3, 4, 0, 7, 3, 7, 1, 4, 3, 4, 1, 7, 3, 7,
    0, 5, 2, 5, 0, 7, 2, 7, 1, 5, 2, 5, 1, 7, 2, 7, 0, 5, 3, 5, 0, 7, 3, 7, 1, 5, 3, 5, 1, 7, 3, 7,
};
}  // Anonymous namespace

InstructionCache::~InstructionCache()
{
  if (m_has_config_callback)
    Config::RemoveConfigChangedCallback(m_config_callback_id);
}

/* The entries of the valid ways, the only ones set: the tables (megabytes) are not swept. */
void Cache::ClearLookups(Memory::MemoryManager& memory)
{
  for (u32 set = 0; set < CACHE_SETS; set++)
  {
    for (u32 way = 0; way < CACHE_WAYS; way++)
    {
      if ((valid[set] & (1 << way)) == 0)
        continue;
      if (addrs[set][way] & CACHE_VMEM_BIT)
        lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = 0;
      else if (addrs[set][way] & CACHE_EXRAM_BIT)
        lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = 0;
      else
        lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = 0;
    }
  }
}

void Cache::Reset()
{
  if (lookup_table)
    ClearLookups(Core::System::GetInstance().GetMemory());
  memset(valid, 0, sizeof(valid));
  memset(plru, 0, sizeof(plru));
  memset(modified, 0, sizeof(modified));
}

void InstructionCache::Reset(JitInterface& jit_interface)
{
  Cache::Reset();
  jit_interface.ClearSafe();
}

Cache::~Cache()
{
  if (lookup_table)
    Common::FreeMemoryPages(lookup_table, m_lookup_size);
}

void Cache::Init(Memory::MemoryManager& memory)
{
  const size_t ram = memory.GetRamSize() >> 5;
  const size_t ex = memory.GetExRamSize() >> 5;
  const size_t size = ram + ex + (memory.GetFakeVMemSize() >> 5);

  /* the tables' set entries go before what they map is forgotten */
  if (lookup_table && size == m_lookup_size)
  {
    ClearLookups(memory);
  }
  else
  {
    if (lookup_table)
      Common::FreeMemoryPages(lookup_table, m_lookup_size);
    lookup_table = static_cast<u8*>(Common::AllocateMemoryPages(size));
    m_lookup_size = lookup_table ? size : 0;
  }
  lookup_table_ex = lookup_table ? lookup_table + ram : nullptr;
  lookup_table_vmem = lookup_table ? lookup_table + ram + ex : nullptr;
  memset(data, 0, sizeof(data));
  memset(addrs, 0, sizeof(addrs));
  memset(valid, 0, sizeof(valid));
  memset(plru, 0, sizeof(plru));
  memset(modified, 0, sizeof(modified));
}

void InstructionCache::Init(Memory::MemoryManager& memory)
{
  if (!m_has_config_callback)
  {
    m_config_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
    m_has_config_callback = true;
  }
  RefreshConfig();

  Cache::Init(memory);
}

void Cache::Store(Memory::MemoryManager& memory, u32 addr)
{
  const u32 set = (addr >> 5) & 0x7f;
  const u32 way = GetCache(memory, addr, true);

  if (way == 0xff)
    return;

  if (valid[set] & (1U << way) && modified[set] & (1U << way))
    memory.CopyToEmu((addr & ~0x1f), data[set][way], 32);
  modified[set] &= ~(1U << way);
}

void Cache::FlushAll(Memory::MemoryManager& memory)
{
  for (size_t set = 0; set < CACHE_SETS; set++)
  {
    for (size_t way = 0; way < CACHE_WAYS; way++)
    {
      if (valid[set] & (1U << way) && modified[set] & (1U << way))
        memory.CopyToEmu(addrs[set][way], data[set][way], 32);
    }
  }

  Reset();
}

void Cache::Invalidate(Memory::MemoryManager& memory, u32 addr)
{
  const u32 set = (addr >> 5) & 0x7f;
  const u32 way = GetCache(memory, addr, true);

  if (way == 0xff)
    return;

  if (valid[set] & (1U << way))
  {
    if (addrs[set][way] & CACHE_VMEM_BIT)
      lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = 0;
    else if (addrs[set][way] & CACHE_EXRAM_BIT)
      lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = 0;
    else
      lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = 0;

    valid[set] &= ~(1U << way);
    modified[set] &= ~(1U << way);
  }
}

void Cache::Flush(Memory::MemoryManager& memory, u32 addr)
{
  const u32 set = (addr >> 5) & 0x7f;
  const u32 way = GetCache(memory, addr, true);

  if (way == 0xff)
    return;

  if (valid[set] & (1U << way))
  {
    if (modified[set] & (1U << way))
      memory.CopyToEmu((addr & ~0x1f), data[set][way], 32);

    if (addrs[set][way] & CACHE_VMEM_BIT)
      lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = 0;
    else if (addrs[set][way] & CACHE_EXRAM_BIT)
      lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = 0;
    else
      lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = 0;

    valid[set] &= ~(1U << way);
    modified[set] &= ~(1U << way);
  }
}

void Cache::Touch(Memory::MemoryManager& memory, u32 addr, bool store)
{
  GetCache(memory, addr, false);
}

u32 Cache::GetCache(Memory::MemoryManager& memory, u32 addr, bool locked)
{
  addr &= ~31;
  u32 set = (addr >> 5) & 0x7f;
  u32 way;

  /* way + 1, 0 for none */
  if (addr & CACHE_VMEM_BIT)
  {
    way = lookup_table_vmem[(addr & memory.GetFakeVMemMask()) >> 5];
  }
  else if (addr & CACHE_EXRAM_BIT)
  {
    way = lookup_table_ex[(addr & memory.GetExRamMask()) >> 5];
  }
  else
  {
    way = lookup_table[(addr & memory.GetRamMask()) >> 5];
  }
  way = way ? way - 1 : 0xff;

  // load to the cache
  if (!locked && way == 0xff)
  {
    // select a way
    if (valid[set] != 0xff)
      way = s_way_from_valid[valid[set]];
    else
      way = s_way_from_plru[plru[set]];

    if (valid[set] & (1 << way))
    {
      // store the cache back to main memory
      if (modified[set] & (1 << way))
        memory.CopyToEmu(addrs[set][way], data[set][way], 32);

      if (addrs[set][way] & CACHE_VMEM_BIT)
        lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = 0;
      else if (addrs[set][way] & CACHE_EXRAM_BIT)
        lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = 0;
      else
        lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = 0;
    }

    // load
    memory.CopyFromEmu(data[set][way], (addr & ~0x1f), 32);

    if (addr & CACHE_VMEM_BIT)
      lookup_table_vmem[(addr & memory.GetFakeVMemMask()) >> 5] = way + 1;
    else if (addr & CACHE_EXRAM_BIT)
      lookup_table_ex[(addr & memory.GetExRamMask()) >> 5] = way + 1;
    else
      lookup_table[(addr & memory.GetRamMask()) >> 5] = way + 1;

    addrs[set][way] = addr;
    valid[set] |= (1 << way);
    modified[set] &= ~(1 << way);
  }

  // update plru
  if (way != 0xff)
    plru[set] = (plru[set] & ~s_plru_mask[way]) | s_plru_value[way];

  return way;
}

void Cache::Read(Memory::MemoryManager& memory, u32 addr, void* buffer, u32 len, bool locked)
{
  auto* value = static_cast<u8*>(buffer);

  while (len > 0)
  {
    const u32 set = (addr >> 5) & 0x7f;
    const u32 way = GetCache(memory, addr, locked);

    u32 offset_in_block = addr - (addr & ~31);
    u32 len_in_block = len < ((addr + 32) & ~31) - addr ? len : ((addr + 32) & ~31) - addr;

    if (way != 0xff)
    {
      memcpy(value, reinterpret_cast<u8*>(data[set][way]) + offset_in_block, len_in_block);
    }
    else
    {
      memory.CopyFromEmu(value, addr, len_in_block);
    }

    addr += len_in_block;
    len -= len_in_block;
    value += len_in_block;
  }
}

void Cache::Write(Memory::MemoryManager& memory, u32 addr, const void* buffer, u32 len, bool locked)
{
  auto* value = static_cast<const u8*>(buffer);

  while (len > 0)
  {
    const u32 set = (addr >> 5) & 0x7f;
    const u32 way = GetCache(memory, addr, locked);

    u32 offset_in_block = addr - (addr & ~31);
    u32 len_in_block = len < ((addr + 32) & ~31) - addr ? len : ((addr + 32) & ~31) - addr;

    if (way != 0xff)
    {
      memcpy(reinterpret_cast<u8*>(data[set][way]) + offset_in_block, value, len_in_block);
      modified[set] |= (1 << way);
    }
    else
    {
      memory.CopyToEmu(addr, value, len_in_block);
    }

    addr += len_in_block;
    len -= len_in_block;
    value += len_in_block;
  }
}

void Cache::DoState(Memory::MemoryManager& memory, PointerWrap& p)
{
  // the entries of the cached blocks go, rather than sweeping the tables
  if (p.IsReadMode())
    ClearLookups(memory);

  p.DoArray(data);
  p.DoArray(plru);
  p.DoArray(valid);
  p.DoArray(addrs);
  p.DoArray(modified);

  if (p.IsReadMode())
  {
    // Recompute lookup tables
    for (u32 set = 0; set < CACHE_SETS; set++)
    {
      for (u32 way = 0; way < CACHE_WAYS; way++)
      {
        if ((valid[set] & (1 << way)) != 0)
        {
          if (addrs[set][way] & CACHE_VMEM_BIT)
            lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = way + 1;
          else if (addrs[set][way] & CACHE_EXRAM_BIT)
            lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = way + 1;
          else
            lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = way + 1;
        }
      }
    }
  }
}

u32 InstructionCache::ReadInstruction(Memory::MemoryManager& memory,
                                      PowerPC::PowerPCState& ppc_state, u32 addr)
{
  if (!HID0(ppc_state).ICE || m_disable_icache)  // instruction cache is disabled
    return memory.Read_U32(addr);

  u32 value;
  Read(memory, addr, &value, sizeof(value), HID0(ppc_state).ILOCK);
  return Common::swap32(value);
}

void InstructionCache::Invalidate(Memory::MemoryManager& memory, JitInterface& jit_interface,
                                  u32 addr)
{
  // Per the 750cl manual, section 3.4.1.5 Instruction Cache Enabling/Disabling (page 137)
  // and section 3.4.2.6 Instruction Cache Block Invalidate (icbi) (page 140), the icbi
  // instruction always invalidates, even if the instruction cache is disabled or locked,
  // and it also invalidates all ways of the corresponding cache set, not just the way corresponding
  // to the given address.
  // (However, the icbi instruction's info on page 432 does not include this information)
  const u32 set = (addr >> 5) & 0x7f;
  for (size_t way = 0; way < 8; way++)
  {
    if (valid[set] & (1U << way))
    {
      if (addrs[set][way] & CACHE_VMEM_BIT)
        lookup_table_vmem[(addrs[set][way] & memory.GetFakeVMemMask()) >> 5] = 0;
      else if (addrs[set][way] & CACHE_EXRAM_BIT)
        lookup_table_ex[(addrs[set][way] & memory.GetExRamMask()) >> 5] = 0;
      else
        lookup_table[(addrs[set][way] & memory.GetRamMask()) >> 5] = 0;
    }
  }
  valid[set] = 0;
  modified[set] = 0;

  // Also tell the JIT that the corresponding address has been invalidated
  jit_interface.InvalidateICacheLine(addr);
}

void InstructionCache::RefreshConfig()
{
  m_disable_icache = Config::Get(Config::MAIN_DISABLE_ICACHE);
}
}  // namespace PowerPC
