// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemArena.h"

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

using PVirtualAlloc2 = PVOID(WINAPI*)(HANDLE Process, PVOID BaseAddress, SIZE_T Size,
                                      ULONG AllocationType, ULONG PageProtection,
                                      MEM_EXTENDED_PARAMETER* ExtendedParameters,
                                      ULONG ParameterCount);

using PMapViewOfFile3 = PVOID(WINAPI*)(HANDLE FileMapping, HANDLE Process, PVOID BaseAddress,
                                       ULONG64 Offset, SIZE_T ViewSize, ULONG AllocationType,
                                       ULONG PageProtection,
                                       MEM_EXTENDED_PARAMETER* ExtendedParameters,
                                       ULONG ParameterCount);

using PUnmapViewOfFileEx = BOOL(WINAPI*)(PVOID BaseAddress, ULONG UnmapFlags);

using PIsApiSetImplemented = BOOL(APIENTRY*)(PCSTR Contract);

namespace Common
{
enum
{
  SINGLE_PAGE_SIZE = 0x1000,
  REGIONS_START = 16384
};

struct WindowsMemoryRegion
{
  u8* m_start;
  size_t m_size;
  bool m_is_mapped;
};

// A free region at index, those from there on moved up
void MemArena::InsertRegion(size_t index, u8* start, size_t size)
{
  if (m_num_regions == m_regions_capacity)
  {
    const size_t capacity = m_regions_capacity ? m_regions_capacity * 2 : REGIONS_START;
    WindowsMemoryRegion* const grown = static_cast<WindowsMemoryRegion*>(
        std::realloc(m_regions, capacity * sizeof(WindowsMemoryRegion)));
    if (!grown)
      std::abort();
    m_regions = grown;
    m_regions_capacity = capacity;
  }
  std::memmove(&m_regions[index + 1], &m_regions[index],
               (m_num_regions - index) * sizeof(WindowsMemoryRegion));
  m_regions[index].m_start = start;
  m_regions[index].m_size = size;
  m_regions[index].m_is_mapped = false;
  m_num_regions++;
}

void MemArena::EraseRegions(size_t index, size_t count)
{
  std::memmove(&m_regions[index], &m_regions[index + count],
               (m_num_regions - index - count) * sizeof(WindowsMemoryRegion));
  m_num_regions -= count;
}

static bool InitWindowsMemoryFunctions(WindowsMemoryFunctions* functions)
{
  DynamicLibrary kernelBase{"KernelBase.dll"};
  if (!kernelBase.IsOpen())
    return false;

  void* const ptr_IsApiSetImplemented = kernelBase.GetSymbolAddress("IsApiSetImplemented");
  if (!ptr_IsApiSetImplemented)
    return false;
  if (!reinterpret_cast<PIsApiSetImplemented>(ptr_IsApiSetImplemented)(
          "api-ms-win-core-memory-l1-1-6"))
    return false;

  functions->m_api_ms_win_core_memory_l1_1_6_handle.Open("api-ms-win-core-memory-l1-1-6.dll");
  functions->m_kernel32_handle.Open("Kernel32.dll");
  if (!functions->m_api_ms_win_core_memory_l1_1_6_handle.IsOpen() ||
      !functions->m_kernel32_handle.IsOpen())
  {
    functions->m_api_ms_win_core_memory_l1_1_6_handle.Close();
    functions->m_kernel32_handle.Close();
    return false;
  }

  void* const address_VirtualAlloc2 =
      functions->m_api_ms_win_core_memory_l1_1_6_handle.GetSymbolAddress("VirtualAlloc2FromApp");
  void* const address_MapViewOfFile3 =
      functions->m_api_ms_win_core_memory_l1_1_6_handle.GetSymbolAddress("MapViewOfFile3FromApp");
  void* const address_UnmapViewOfFileEx =
      functions->m_kernel32_handle.GetSymbolAddress("UnmapViewOfFileEx");
  if (address_VirtualAlloc2 && address_MapViewOfFile3 && address_UnmapViewOfFileEx)
  {
    functions->m_address_VirtualAlloc2 = address_VirtualAlloc2;
    functions->m_address_MapViewOfFile3 = address_MapViewOfFile3;
    functions->m_address_UnmapViewOfFileEx = address_UnmapViewOfFileEx;
    return true;
  }

  // at least one function is not available, use legacy logic
  functions->m_api_ms_win_core_memory_l1_1_6_handle.Close();
  functions->m_kernel32_handle.Close();
  return false;
}

MemArena::MemArena()
{
  // Check if VirtualAlloc2 and MapViewOfFile3 are available, which provide functionality to reserve
  // a memory region no other allocation may occupy while still allowing us to allocate and map
  // stuff within it. If they're not available we'll instead fall back to the 'legacy' logic and
  // just hope that nothing allocates in our address range.
  InitWindowsMemoryFunctions(&m_memory_functions);
}

MemArena::~MemArena()
{
  ReleaseMemoryRegion();
  ReleaseSHMSegment();
}

static DWORD GetHighDWORD(u64 value)
{
  return static_cast<DWORD>(value >> 32);
}

static DWORD GetLowDWORD(u64 value)
{
  return static_cast<DWORD>(value);
}

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  char name[256];
  snprintf(name, sizeof(name), "%.*s.%lu", static_cast<int>(base_name.size()), base_name.data(),
           static_cast<unsigned long>(GetCurrentProcessId()));
  m_memory_handle =
      CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, GetHighDWORD(size),
                        GetLowDWORD(size), UTF8ToTStr(name).c_str());
}

void MemArena::ReleaseSHMSegment()
{
  if (!m_memory_handle)
    return;
  CloseHandle(m_memory_handle);
  m_memory_handle = nullptr;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  const u64 off = static_cast<u64>(offset);
  return MapViewOfFileEx(m_memory_handle, FILE_MAP_ALL_ACCESS, GetHighDWORD(off), GetLowDWORD(off),
                         size, nullptr);
}

void MemArena::ReleaseView(void* view, size_t size)
{
  UnmapViewOfFile(view);
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  if (m_reserved_region)
  {
    PanicAlert("Tried to reserve a second memory region from the same MemArena.");
    return nullptr;
  }

  u8* base;
  if (m_memory_functions.m_api_ms_win_core_memory_l1_1_6_handle.IsOpen())
  {
    base = static_cast<u8*>(reinterpret_cast<PVirtualAlloc2>(
        m_memory_functions.m_address_VirtualAlloc2)(nullptr, nullptr, memory_size,
                                                    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                    PAGE_NOACCESS, nullptr, 0));
    if (base)
    {
      m_reserved_region = base;
      InsertRegion(0, base, memory_size);
    }
    else
    {
      PanicAlert("Failed to map enough memory space: %s", GetLastErrorString().c_str());
    }
  }
  else
  {
    NOTICE_LOG(MEMMAP, "VirtualAlloc2 and/or MapViewFromFile3 unavailable. "
                       "Falling back to legacy memory mapping.");
    base = static_cast<u8*>(VirtualAlloc(nullptr, memory_size, MEM_RESERVE, PAGE_READWRITE));
    if (base)
      VirtualFree(base, 0, MEM_RELEASE);
    else
      PanicAlert("Failed to find enough memory space: %s", GetLastErrorString().c_str());
  }

  return base;
}

void MemArena::ReleaseMemoryRegion()
{
  if (m_memory_functions.m_api_ms_win_core_memory_l1_1_6_handle.IsOpen() && m_reserved_region)
  {
    // user should have unmapped everything by this point, check if that's true and yell if not
    // (it indicates a bug in the emulated memory mapping logic)
    size_t mapped_region_count = 0;
    size_t i;
    for (i = 0; i < m_num_regions; i++)
    {
      if (m_regions[i].m_is_mapped)
        ++mapped_region_count;
    }

    if (mapped_region_count > 0)
    {
      PanicAlert("Error while releasing fastmem region: %" PRIu64 " regions are still mapped!",
                 (u64)mapped_region_count);
    }

    // one placeholder again, the single pages left split by UnmapFromMemoryRegion joined
    if (mapped_region_count == 0 && m_num_regions)
    {
      CoalesceFreeRegions(0, m_regions[m_num_regions - 1].m_start +
                                 m_regions[m_num_regions - 1].m_size);
    }

    // then free memory
    VirtualFree(m_reserved_region, 0, MEM_RELEASE);
    m_reserved_region = nullptr;
    std::free(m_regions);
    m_regions = nullptr;
    m_num_regions = 0;
    m_regions_capacity = 0;
  }
}

// The free region at index and the free ones after it, up to end, joined into one placeholder;
// false when the system would not
bool MemArena::CoalesceFreeRegions(size_t index, const u8* end)
{
  WindowsMemoryRegion* const regions = m_regions;
  size_t last = index;
  size_t total = regions[index].m_size;

  while (regions[index].m_start + total < end && last + 1 < m_num_regions &&
         !regions[last + 1].m_is_mapped)
  {
    last++;
    total += regions[last].m_size;
  }
  if (last == index)
    return true;
  if (!VirtualFree(regions[index].m_start, total, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
  {
    NOTICE_LOG(MEMMAP, "Region coalescing failed: %s", GetLastErrorString().c_str());
    return false;
  }
  regions[index].m_size = total;
  EraseRegions(index + 1, last - index);
  return true;
}

WindowsMemoryRegion* MemArena::EnsureSplitRegionForMapping(void* start_address, size_t size)
{
  u8* const address = static_cast<u8*>(start_address);
  size_t index, lo, hi;
  u8* mapping_address;
  size_t mapping_size, size_before, minimum_size;

  if (!m_num_regions)
  {
    NOTICE_LOG(MEMMAP, "Tried to map a memory region without reserving a memory block first.");
    return nullptr;
  }

  // the last region that starts at or before the given address
  lo = 0;
  hi = m_num_regions;
  while (lo < hi)
  {
    const size_t mid = lo + (hi - lo) / 2;
    if (address < m_regions[mid].m_start)
      hi = mid;
    else
      lo = mid + 1;
  }
  if (lo == 0)
  {
    // this should never happen, implies that the given address is before the start of the
    // reserved memory block
    NOTICE_LOG(MEMMAP, "Invalid address %p given to map.", static_cast<const void*>(address));
    return nullptr;
  }
  index = lo - 1;

  if (m_regions[index].m_is_mapped)
  {
    NOTICE_LOG(MEMMAP,
               "Address to map %p with a size of 0x%" PRIx64 " overlaps with existing mapping "
               "at %p.",
               static_cast<const void*>(address), (u64)size,
               static_cast<const void*>(m_regions[index].m_start));
    return nullptr;
  }

  // single pages unmapped are left split: joined here when what is mapped needs more than one
  if (m_regions[index].m_start + m_regions[index].m_size < address + size)
  {
    if (!CoalesceFreeRegions(index, address + size))
      return nullptr;
  }

  mapping_address = m_regions[index].m_start;
  mapping_size = m_regions[index].m_size;
  if (mapping_address == address)
  {
    // if this region is already split up correctly we don't have to do anything
    if (mapping_size == size)
      return &m_regions[index];

    // if this region is smaller than the requested size we can't map
    if (mapping_size < size)
    {
      NOTICE_LOG(MEMMAP,
                 "Not enough free space at address %p to map 0x%" PRIx64 " bytes (0x%" PRIx64
                 " available).",
                 static_cast<const void*>(mapping_address), (u64)size, (u64)mapping_size);
      return nullptr;
    }

    // split region
    if (!VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      NOTICE_LOG(MEMMAP, "Region splitting failed: %s", GetLastErrorString().c_str());
      return nullptr;
    }

    // update tracked mappings and return the first of the two
    m_regions[index].m_size = size;
    InsertRegion(index + 1, address + size, mapping_size - size);
    return &m_regions[index];
  }

  ASSERT(mapping_address < address);

  // is there enough space to map this?
  size_before = static_cast<size_t>(address - mapping_address);
  minimum_size = size + size_before;
  if (mapping_size < minimum_size)
  {
    NOTICE_LOG(MEMMAP,
               "Not enough free space at address %p to map memory region (need 0x%" PRIx64 " "
               "bytes, but only 0x%" PRIx64 " available).",
               static_cast<const void*>(address), (u64)minimum_size, (u64)mapping_size);
    return nullptr;
  }

  // split region
  if (!VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
  {
    NOTICE_LOG(MEMMAP, "Region splitting failed: %s", GetLastErrorString().c_str());
    return nullptr;
  }

  // two regions now, or three with what is left after: the one mapped is the second
  m_regions[index].m_size = size_before;
  if (mapping_size != minimum_size)
    InsertRegion(index + 1, address + size, mapping_size - minimum_size);
  InsertRegion(index + 1, address, size);
  return &m_regions[index + 1];
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  void* result;

  if (m_memory_functions.m_api_ms_win_core_memory_l1_1_6_handle.IsOpen())
  {
    WindowsMemoryRegion* const region = EnsureSplitRegionForMapping(base, size);
    if (!region)
    {
      PanicAlert("Splitting memory region failed.");
      return nullptr;
    }

    result = reinterpret_cast<PMapViewOfFile3>(m_memory_functions.m_address_MapViewOfFile3)(
        m_memory_handle, nullptr, base, offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
        nullptr, 0);
    if (result)
    {
      region->m_is_mapped = true;
    }
    else
    {
      PanicAlert("Mapping memory region failed: %s", GetLastErrorString().c_str());

      // revert the split, if any
      JoinRegionsAfterUnmap(base, size, true);

      return nullptr;
    }
  }
  else
  {
    result =
        MapViewOfFileEx(m_memory_handle, FILE_MAP_ALL_ACCESS, 0, (DWORD)((u64)offset), size, base);

    if (!result)
      return nullptr;
  }

  if (!writeable)
  {
    // If we want to use PAGE_READONLY for now while still being able to switch to PAGE_READWRITE
    // later, we have to call MapViewOfFile with PAGE_READWRITE and then switch to PAGE_READONLY.
    ChangeMappingProtection(base, size, writeable);
  }

  return result;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  DWORD old_protect;
  const int retval =
      VirtualProtect(view, size, writeable ? PAGE_READWRITE : PAGE_READONLY, &old_protect);
  if (retval == 0)
    PanicAlert("VirtualProtect failed: %s", GetLastErrorString().c_str());
  return retval != 0;
}

// The region at the address is free again; joined with the free ones beside it if join
bool MemArena::JoinRegionsAfterUnmap(void* start_address, size_t size, bool join)
{
  u8* const address = static_cast<u8*>(start_address);
  size_t index, lo, hi, first, count, total_size;

  if (!m_num_regions)
  {
    NOTICE_LOG(MEMMAP, "Tried to unmap a memory region without reserving a memory block first.");
    return false;
  }

  // there should be a mapping that matches the request exactly, find it
  lo = 0;
  hi = m_num_regions;
  while (lo < hi)
  {
    const size_t mid = lo + (hi - lo) / 2;
    if (m_regions[mid].m_start < address)
      lo = mid + 1;
    else
      hi = mid;
  }
  index = lo;
  if (index == m_num_regions || m_regions[index].m_start != address ||
      m_regions[index].m_size != size)
  {
    // didn't find it, we were given bogus input
    NOTICE_LOG(MEMMAP, "Invalid address/size given to unmap.");
    return false;
  }
  m_regions[index].m_is_mapped = false;
  if (!join)
    return true;

  // with the free one before it and the free one after it, whichever there are
  first = index;
  count = 1;
  total_size = size;
  if (index > 0 && !m_regions[index - 1].m_is_mapped)
  {
    first = index - 1;
    count++;
    total_size += m_regions[index - 1].m_size;
  }
  if (index + 1 < m_num_regions && !m_regions[index + 1].m_is_mapped)
  {
    count++;
    total_size += m_regions[index + 1].m_size;
  }
  if (count == 1)
    return true;
  if (!VirtualFree(m_regions[first].m_start, total_size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
  {
    NOTICE_LOG(MEMMAP, "Region coalescing failed: %s", GetLastErrorString().c_str());
    return false;
  }
  m_regions[first].m_size = total_size;
  EraseRegions(first + 1, count - 1);
  return true;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  if (m_memory_functions.m_api_ms_win_core_memory_l1_1_6_handle.IsOpen())
  {
    if (reinterpret_cast<PUnmapViewOfFileEx>(m_memory_functions.m_address_UnmapViewOfFileEx)(
            view, MEM_PRESERVE_PLACEHOLDER))
    {
      // A single page, as the game's page table maps them, stays split: the next mapping there
      // replaces its placeholder as it is, with no call to split it and none here to join it.
      // EnsureSplitRegionForMapping joins them when something larger is mapped over them.
      if (!JoinRegionsAfterUnmap(view, size, size > SINGLE_PAGE_SIZE))
        PanicAlert("Joining memory region failed.");
    }
    else
    {
      PanicAlert("Unmapping memory region failed: %s", GetLastErrorString().c_str());
    }
    return;
  }

  UnmapViewOfFile(view);
}

size_t MemArena::GetPageSize() const
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);

  if (!m_memory_functions.m_address_MapViewOfFile3)
  {
    // In this case, we can only map pages that are 64K aligned.
    // See https://devblogs.microsoft.com/oldnewthing/20031008-00/?p=42223
    return si.dwPageSize > 64 * 1024 ? size_t(si.dwPageSize) : size_t(64 * 1024);
  }

  return si.dwPageSize;
}

LazyMemoryRegion::LazyMemoryRegion()
{
  InitWindowsMemoryFunctions(&m_memory_functions);
}

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);

  if (size == 0)
    return nullptr;

  if (!m_memory_functions.m_api_ms_win_core_memory_l1_1_6_handle.IsOpen())
    return nullptr;

  // reserve block of memory
  const size_t memory_size = Common::AlignUp(size, BLOCK_SIZE);
  const size_t block_count = memory_size / BLOCK_SIZE;
  u8* memory =
      static_cast<u8*>(reinterpret_cast<PVirtualAlloc2>(m_memory_functions.m_address_VirtualAlloc2)(
          nullptr, nullptr, memory_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
          nullptr, 0));
  if (!memory)
  {
    NOTICE_LOG(MEMMAP, "Memory reservation of %" PRIu64 " bytes failed.", (u64)size);
    return nullptr;
  }

  // split into individual block-sized regions
  for (size_t i = 0; i < block_count - 1; ++i)
  {
    if (!VirtualFree(memory + i * BLOCK_SIZE, BLOCK_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      NOTICE_LOG(MEMMAP, "Region splitting failed: %s", GetLastErrorString().c_str());

      // release every split block as well as the remaining unsplit one
      for (size_t j = 0; j < i + 1; ++j)
        VirtualFree(memory + j * BLOCK_SIZE, 0, MEM_RELEASE);

      return nullptr;
    }
  }

  m_memory = memory;
  m_size = memory_size;

  // allocate a single block of real memory in the page file
  HANDLE zero_block = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY,
                                        GetHighDWORD(BLOCK_SIZE), GetLowDWORD(BLOCK_SIZE), nullptr);
  if (zero_block == nullptr)
  {
    NOTICE_LOG(MEMMAP, "CreateFileMapping() failed for zero block: %s",
               GetLastErrorString().c_str());
    Release();
    return nullptr;
  }

  m_zero_block = zero_block;

  // map the zero page into every block
  for (size_t i = 0; i < block_count; ++i)
  {
    void* result = reinterpret_cast<PMapViewOfFile3>(m_memory_functions.m_address_MapViewOfFile3)(
        zero_block, nullptr, memory + i * BLOCK_SIZE, 0, BLOCK_SIZE, MEM_REPLACE_PLACEHOLDER,
        PAGE_READONLY, nullptr, 0);
    if (!result)
    {
      NOTICE_LOG(MEMMAP, "Mapping the zero block failed: %s", GetLastErrorString().c_str());
      Release();
      return nullptr;
    }
  }

  m_writable_block_handles.resize(block_count, nullptr);

  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  u8* const memory = static_cast<u8*>(m_memory);

  // reset every writable block back to the zero block
  for (size_t i = 0; i < m_writable_block_handles.size(); ++i)
  {
    if (m_writable_block_handles[i] == nullptr)
      continue;

    // unmap the writable block
    if (!reinterpret_cast<PUnmapViewOfFileEx>(m_memory_functions.m_address_UnmapViewOfFileEx)(
            memory + i * BLOCK_SIZE, MEM_PRESERVE_PLACEHOLDER))
    {
      PanicAlert("Failed to unmap the writable block: %s", GetLastErrorString().c_str());
    }

    // free the writable block
    if (!CloseHandle(m_writable_block_handles[i]))
    {
      PanicAlert("Failed to free the writable block: %s", GetLastErrorString().c_str());
    }
    m_writable_block_handles[i] = nullptr;

    // map the zero block
    void* map_result =
        reinterpret_cast<PMapViewOfFile3>(m_memory_functions.m_address_MapViewOfFile3)(
            m_zero_block, nullptr, memory + i * BLOCK_SIZE, 0, BLOCK_SIZE, MEM_REPLACE_PLACEHOLDER,
            PAGE_READONLY, nullptr, 0);
    if (!map_result)
    {
      PanicAlert("Failed to re-map the zero block: %s", GetLastErrorString().c_str());
    }
  }
}

void LazyMemoryRegion::Release()
{
  if (m_memory)
  {
    // unmap all pages and release the not-zero block handles
    u8* const memory = static_cast<u8*>(m_memory);
    for (size_t i = 0; i < m_writable_block_handles.size(); ++i)
    {
      reinterpret_cast<PUnmapViewOfFileEx>(m_memory_functions.m_address_UnmapViewOfFileEx)(
          memory + i * BLOCK_SIZE, MEM_PRESERVE_PLACEHOLDER);
      if (m_writable_block_handles[i])
      {
        CloseHandle(m_writable_block_handles[i]);
        m_writable_block_handles[i] = nullptr;
      }
    }
  }
  if (m_zero_block)
  {
    CloseHandle(m_zero_block);
    m_zero_block = nullptr;
  }
  if (m_memory)
  {
    u8* const memory = static_cast<u8*>(m_memory);
    const size_t block_count = m_size / BLOCK_SIZE;
    for (size_t i = 0; i < block_count; ++i)
      VirtualFree(memory + i * BLOCK_SIZE, 0, MEM_RELEASE);
    m_memory = nullptr;
    m_size = 0;
  }
}

void LazyMemoryRegion::MakeMemoryBlockWritable(size_t block_index)
{
  u8* const memory = static_cast<u8*>(m_memory);

  // unmap the zero block
  if (!reinterpret_cast<PUnmapViewOfFileEx>(m_memory_functions.m_address_UnmapViewOfFileEx)(
          memory + block_index * BLOCK_SIZE, MEM_PRESERVE_PLACEHOLDER))
  {
    PanicAlert("Failed to unmap the zero block: %s", GetLastErrorString().c_str());
    return;
  }

  // allocate a fresh block to map
  HANDLE block = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   GetHighDWORD(BLOCK_SIZE), GetLowDWORD(BLOCK_SIZE), nullptr);
  if (block == nullptr)
  {
    PanicAlert("CreateFileMapping() failed for writable block: %s", GetLastErrorString().c_str());
    return;
  }

  // map the new block
  void* map_result = reinterpret_cast<PMapViewOfFile3>(m_memory_functions.m_address_MapViewOfFile3)(
      block, nullptr, memory + block_index * BLOCK_SIZE, 0, BLOCK_SIZE, MEM_REPLACE_PLACEHOLDER,
      PAGE_READWRITE, nullptr, 0);
  if (!map_result)
  {
    PanicAlert("Failed to map the writable block: %s", GetLastErrorString().c_str());
    CloseHandle(block);
    return;
  }

  m_writable_block_handles[block_index] = block;
}

}  // namespace Common
