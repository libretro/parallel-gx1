// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemArena.h"

#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace Common
{
MemArena::MemArena() = default;
MemArena::~MemArena() = default;

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  char file_name[256];
  snprintf(file_name, sizeof(file_name), "/%.*s.%d", static_cast<int>(base_name.size()),
           base_name.data(), static_cast<int>(getpid()));
  m_shm_fd = shm_open(file_name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (m_shm_fd == -1)
  {
    ERROR_LOG(MEMMAP, "shm_open failed: %s", strerror(errno));
    return;
  }
  shm_unlink(file_name);
  if (ftruncate(m_shm_fd, size) < 0)
    ERROR_LOG(MEMMAP, "Failed to allocate low memory space");
}

void MemArena::ReleaseSHMSegment()
{
  close(m_shm_fd);
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  void* retval = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, offset);
  if (retval == MAP_FAILED)
  {
    NOTICE_LOG(MEMMAP, "mmap failed");
    return nullptr;
  }
  else
  {
    return retval;
  }
}

void MemArena::ReleaseView(void* view, size_t size)
{
  munmap(view, size);
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  const int flags = MAP_ANON | MAP_PRIVATE;
  void* base = mmap(nullptr, memory_size, PROT_NONE, flags, -1, 0);
  if (base == MAP_FAILED)
  {
    PanicAlert("Failed to map enough memory space: %s", LastStrerrorString().c_str());
    return nullptr;
  }
  m_reserved_region = base;
  m_reserved_region_size = memory_size;
  return static_cast<u8*>(base);
}

void MemArena::ReleaseMemoryRegion()
{
  if (m_reserved_region)
  {
    munmap(m_reserved_region, m_reserved_region_size);
    m_reserved_region = nullptr;
  }
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  int prot = PROT_READ;
  if (writeable)
    prot |= PROT_WRITE;

  void* retval = mmap(base, size, prot, MAP_SHARED | MAP_FIXED, m_shm_fd, offset);
  if (retval == MAP_FAILED)
  {
    NOTICE_LOG(MEMMAP, "mmap failed");
    return nullptr;
  }
  else
  {
    return retval;
  }
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  int prot = PROT_READ;
  if (writeable)
    prot |= PROT_WRITE;

  int retval = mprotect(view, size, prot);
  if (retval != 0)
    NOTICE_LOG(MEMMAP, "mprotect failed");
  return retval == 0;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  void* retval = mmap(view, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (retval == MAP_FAILED)
    NOTICE_LOG(MEMMAP, "mmap failed");
}

size_t MemArena::GetPageSize() const
{
  return sysconf(_SC_PAGESIZE);
}

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

#if !defined MAP_NORESERVE && (defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__)
// BSD does not implement MAP_NORESERVE, so define the flag to nothing.
// See https://reviews.freebsd.org/rS273250
#define MAP_NORESERVE 0
#endif

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);

  if (size == 0)
    return nullptr;

  void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (memory == MAP_FAILED)
  {
    NOTICE_LOG(MEMMAP, "Memory allocation of %" PRIu64 " bytes failed.", (u64)size);
    return nullptr;
  }

  m_memory = memory;
  m_size = size;

  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);

  void* new_memory = mmap(m_memory, m_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  ASSERT(new_memory == m_memory);
}

void LazyMemoryRegion::Release()
{
  if (m_memory)
  {
    munmap(m_memory, m_size);
    m_memory = nullptr;
    m_size = 0;
  }
}

}  // namespace Common
