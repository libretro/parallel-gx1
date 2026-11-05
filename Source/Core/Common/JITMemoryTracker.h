// Copyright 2022 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include <retro_atomic.h>

#include "Common/Quiesce.h"

namespace Common
{
// Tracks the JIT code regions whose write and execute permissions are toggled around code
// emission. Each JIT emits into its own region on its own thread, so a region's nest count is
// that thread's alone; the set of regions is shared, and kept in atomic slots.
class JITMemoryTracker final
{
public:
  JITMemoryTracker();
  ~JITMemoryTracker();

  void RegisterJITRegion(void* ptr, size_t size);
  void UnregisterJITRegion(void* ptr);

  void JITRegionWriteEnableExecuteDisable(void* ptr);
  void JITRegionWriteDisableExecuteEnable(void* ptr);

private:
  struct JITRegionInfo
  {
    quiesce_node_t node;  // first: the free callback casts it back
    void* start_ptr;
    size_t size;
    int nest_counter;
  };
  static void FreeRegion(quiesce_node_t* node);

  // Only inside m_quiesce.
  JITRegionInfo* FindRegion(void* ptr);

  static constexpr size_t MAX_REGIONS = 64;
  std::array<retro_atomic_ptr_t, MAX_REGIONS> m_jit_regions{};
  quiesce_t m_quiesce{};
};
}  // namespace Common
