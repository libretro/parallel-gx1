// Copyright 2022 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/JITMemoryTracker.h"

#include <cinttypes>

#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"

namespace Common
{
JITMemoryTracker::JITMemoryTracker() = default;

JITMemoryTracker::~JITMemoryTracker()
{
  for (auto& slot : m_jit_regions)
    delete static_cast<JITRegionInfo*>(retro_atomic_exchange_ptr(&slot, nullptr));
  quiesce_deinit(&m_quiesce);
}

void JITMemoryTracker::FreeRegion(quiesce_node_t* node)
{
  delete reinterpret_cast<JITRegionInfo*>(node);
}

void JITMemoryTracker::RegisterJITRegion(void* ptr, size_t size)
{
  quiesce_enter(&m_quiesce);
  const bool known = FindRegion(ptr) != nullptr;
  quiesce_exit(&m_quiesce);
  if (known)
  {
    PanicAlert("JITMemoryTracker: region %p already registered", static_cast<const void*>(ptr));
    return;
  }

  auto* info = new JITRegionInfo{{}, ptr, size, 0};
  for (auto& slot : m_jit_regions)
  {
    if (!retro_atomic_load_acquire_ptr(&slot) && retro_atomic_cas_ptr(&slot, nullptr, info))
      return;
  }
  PanicAlert("JITMemoryTracker: more than %" PRIu64 " JIT regions", (u64)MAX_REGIONS);
  delete info;
}

void JITMemoryTracker::UnregisterJITRegion(void* ptr)
{
  for (auto& slot : m_jit_regions)
  {
    auto* info = static_cast<JITRegionInfo*>(retro_atomic_load_acquire_ptr(&slot));
    if (info && info->start_ptr == ptr && retro_atomic_cas_ptr(&slot, info, nullptr))
    {
      quiesce_retire(&m_quiesce, &info->node, FreeRegion);
      return;
    }
  }
}

JITMemoryTracker::JITRegionInfo* JITMemoryTracker::FindRegion(void* ptr)
{
  JITRegionInfo* containing = nullptr;
  for (auto& slot : m_jit_regions)
  {
    auto* info = static_cast<JITRegionInfo*>(retro_atomic_load_acquire_ptr(&slot));
    if (!info)
      continue;
    if (info->start_ptr == ptr)
      return info;
    void* region_end = static_cast<void*>(static_cast<u8*>(info->start_ptr) + info->size);
    if (!containing && ptr >= info->start_ptr && ptr <= region_end)
      containing = info;
  }
  return containing;
}

void JITMemoryTracker::JITRegionWriteEnableExecuteDisable(void* ptr)
{
  quiesce_enter(&m_quiesce);
  JITRegionInfo* info = FindRegion(ptr);

  if (info)
  {
    if (info->nest_counter == 0)
      UnWriteProtectMemory(info->start_ptr, info->size, false);

    info->nest_counter++;
  }
  quiesce_exit(&m_quiesce);
}

void JITMemoryTracker::JITRegionWriteDisableExecuteEnable(void* ptr)
{
  quiesce_enter(&m_quiesce);
  JITRegionInfo* info = FindRegion(ptr);

  if (info)
  {
    info->nest_counter--;

    if (info->nest_counter < 0)
      PanicAlert("JITMemoryTracker: Nest counter underflow for region %p",
                 static_cast<const void*>(ptr));
    else if (info->nest_counter == 0)
      WriteProtectMemory(info->start_ptr, info->size, true);
  }
  quiesce_exit(&m_quiesce);
}
}  // namespace Common
