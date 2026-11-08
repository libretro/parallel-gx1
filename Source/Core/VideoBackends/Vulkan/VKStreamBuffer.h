// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"

namespace Vulkan
{
class StreamBuffer
{
public:
  StreamBuffer() = default;
  ~StreamBuffer();
  StreamBuffer(const StreamBuffer&) = delete;
  StreamBuffer& operator=(const StreamBuffer&) = delete;

  // Allocates the buffer; false when it cannot be had.
  // heap_usage: VK_HEAP_UPLOAD, or VK_HEAP_UPLOAD_CACHED for memory written in place
  bool Init(VkBufferUsageFlags usage, u32 size, enum vk_heap_usage heap_usage = VK_HEAP_UPLOAD);

  VkBuffer GetBuffer() const { return m_buffer; }
  u8* GetHostPointer() const { return m_host_pointer; }
  u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  u32 GetCurrentSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_current_offset; }
  bool ReserveMemory(u32 num_bytes, u32 alignment);
  void CommitMemory(u32 final_num_bytes);

private:
  bool AllocateBuffer();
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();

  // Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
  bool WaitForClearSpace(u32 num_bytes);

  VkBufferUsageFlags m_usage = 0;
  enum vk_heap_usage m_heap_usage = VK_HEAP_UPLOAD;
  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_gpu_position = 0;
  u32 m_last_allocation_size = 0;

  VkBuffer m_buffer = VK_NULL_HANDLE;
  vk_heap_alloc_t m_alloc = {};
  u8* m_host_pointer = nullptr;

  // The fences of the command buffers still using the buffer, oldest first, and how far into it
  // each had used: a ring, as no more than the command buffers in flight are ever tracked.
  enum
  {
    TRACKED_FENCES = 32
  };
  struct TrackedFence
  {
    u64 counter;
    u32 offset;
  };
  TrackedFence& Tracked(u32 i) { return m_tracked[(m_tracked_head + i) & (TRACKED_FENCES - 1)]; }
  void DropTracked(u32 n)
  {
    m_tracked_head = (m_tracked_head + n) & (TRACKED_FENCES - 1);
    m_num_tracked -= n;
  }
  TrackedFence m_tracked[TRACKED_FENCES] = {};
  u32 m_tracked_head = 0;
  u32 m_num_tracked = 0;
};

}  // namespace Vulkan
