// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"

namespace Common
{
// Everything that needs to generate code should inherit from this.
// You get memory management for free, plus, you can use all emitter functions without
// having to prefix them with gen-> or something similar.
// Example implementation:
// class JIT : public CodeBlock<ARMXEmitter> {}
template <class T, bool executable = true>
class CodeBlock : public T
{
private:
  // A privately used function to set the executable RAM space to something invalid.
  // For debugging usefulness it should be used to set the RAM to a host specific breakpoint
  // instruction. Sets the first size bytes of the region.
  virtual void PoisonMemory(size_t size) = 0;

  // How far into the region code may have been written since it was last poisoned: everything
  // written lies below a pointer this block's emitter wrote up to, or was handed, so a clear
  // poisons that much and leaves the untouched rest, untouched since, alone.
  size_t m_dirty_size = 0;

  void NoteCodePtr(const u8* ptr)
  {
    const u8* exec_ptr = ptr;
    if (writable_region_diff && ptr >= region + writable_region_diff &&
        ptr <= region + writable_region_diff + region_size)
    {
      exec_ptr = ptr - writable_region_diff;
    }
    if (exec_ptr >= region && exec_ptr <= region + region_size &&
        static_cast<size_t>(exec_ptr - region) > m_dirty_size)
    {
      m_dirty_size = static_cast<size_t>(exec_ptr - region);
    }
  }

protected:
  u8* region = nullptr;
  // Size of region we can use.
  size_t region_size = 0;
  // Original size of the region we allocated.
  size_t total_region_size = 0;
  // Offset from executable region to writable region (0 when not dual-mapped).
  ptrdiff_t writable_region_diff = 0;

  bool m_is_child = false;
  std::vector<CodeBlock*> m_children;

public:
  CodeBlock() = default;
  virtual ~CodeBlock()
  {
    if (region)
      FreeCodeSpace();
  }
  CodeBlock(const CodeBlock&) = delete;
  CodeBlock& operator=(const CodeBlock&) = delete;
  CodeBlock(CodeBlock&&) = delete;
  CodeBlock& operator=(CodeBlock&&) = delete;

  // Call this before you generate any code.
  void AllocCodeSpace(size_t size)
  {
    region_size = size;
    total_region_size = size;
    m_dirty_size = 0;
    if constexpr (executable)
      region = static_cast<u8*>(Common::AllocateExecutableMemory(total_region_size));
    else
      region = static_cast<u8*>(Common::AllocateMemoryPages(total_region_size));
    T::SetCodePtr(region, region + size);

#ifdef IPHONEOS
    if constexpr (executable)
      writable_region_diff = Common::AllocateWritableRegionAndGetDiff(region, size);
#endif

    T::SetWritableRegionDiff(writable_region_diff);
  }

  // Always clear code space with breakpoints, so that if someone accidentally executes
  // uninitialized, it just breaks into the debugger.
  void ClearCodeSpace()
  {
    NoteCodePtr(T::GetCodePtr());
    PoisonMemory(m_dirty_size);
    m_dirty_size = 0;
    T::SetCodePtr(region, region + region_size);
  }

  // The emitter's own, noting where this block's code may have been written up to: the pointer
  // left behind, and the one taken up, which another emitter may have written up to.
  void SetCodePtr(u8* ptr, u8* end)
  {
    NoteCodePtr(T::GetCodePtr());
    NoteCodePtr(ptr);
    T::SetCodePtr(ptr, end);
  }
  void SetCodePtr(u8* ptr, u8* end, bool write_failed)
  {
    NoteCodePtr(T::GetCodePtr());
    NoteCodePtr(ptr);
    T::SetCodePtr(ptr, end, write_failed);
  }

  // Call this when shutting down. Don't rely on the destructor, even though it'll do the job.
  void FreeCodeSpace()
  {
    ASSERT(!m_is_child);
#ifdef IPHONEOS
    if constexpr (executable)
    {
      Common::FreeWritableRegion(region, total_region_size, writable_region_diff);
      Common::FreeExecutableMemory(region, total_region_size);
    }
    else
#endif
    {
      Common::FreeMemoryPages(region, total_region_size);
    }
    region = nullptr;
    region_size = 0;
    total_region_size = 0;
    writable_region_diff = 0;
    for (CodeBlock* child : m_children)
    {
      child->region = nullptr;
      child->region_size = 0;
      child->total_region_size = 0;
      child->writable_region_diff = 0;
    }
  }

  bool IsInSpace(const u8* ptr) const { return ptr >= region && ptr < (region + region_size); }
  bool IsInSpaceOrChildSpace(const u8* ptr) const
  {
    return ptr >= region && ptr < (region + total_region_size);
  }
  u8* GetRegionPtr() { return region; }
  ptrdiff_t GetWritableRegionDiff() const { return writable_region_diff; }
  void WriteProtect(bool allow_execute)
  {
    Common::WriteProtectMemory(region, region_size, allow_execute);
  }
  void UnWriteProtect(bool allow_execute)
  {
    Common::UnWriteProtectMemory(region, region_size, allow_execute);
  }
  void ResetCodePtr()
  {
    NoteCodePtr(T::GetCodePtr());
    T::SetCodePtr(region, region + region_size);
  }
  size_t GetSpaceLeft() const
  {
    ASSERT(static_cast<size_t>(T::GetCodePtr() - region) < region_size);
    return region_size - (T::GetCodePtr() - region);
  }

  bool IsAlmostFull() const
  {
    // This should be bigger than the biggest block ever.
    return GetSpaceLeft() < 0x10000;
  }

  bool HasChildren() const { return region_size != total_region_size; }
  u8* AllocChildCodeSpace(size_t child_size)
  {
    ASSERT_MSG(DYNA_REC, child_size <= GetSpaceLeft(), "Insufficient space for child allocation.");
    u8* child_region = region + region_size - child_size;
    region_size -= child_size;
    if (m_dirty_size > region_size)
      m_dirty_size = region_size;
    ResetCodePtr();
    return child_region;
  }
  void AddChildCodeSpace(CodeBlock* child, size_t child_size)
  {
    // what of the child's part was dirty in this block stays dirty in the child's
    NoteCodePtr(T::GetCodePtr());
    const size_t child_offset = region_size - child_size;
    const size_t child_dirty = m_dirty_size > child_offset ? m_dirty_size - child_offset : 0;
    u8* child_region = AllocChildCodeSpace(child_size);
    child->m_is_child = true;
    child->region = child_region;
    child->region_size = child_size;
    child->total_region_size = child_size;
    child->writable_region_diff = writable_region_diff;
    child->m_dirty_size = child_dirty;
    child->ResetCodePtr();
    m_children.emplace_back(child);
  }
};
}  // namespace Common
