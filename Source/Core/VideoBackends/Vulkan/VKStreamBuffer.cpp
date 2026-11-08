// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKStreamBuffer.h"

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{

StreamBuffer::~StreamBuffer()
{
  if (m_buffer != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);
}

bool StreamBuffer::Init(VkBufferUsageFlags usage, u32 size, enum vk_heap_usage heap_usage)
{
  m_usage = usage;
  m_heap_usage = heap_usage;
  m_size = size;
  return AllocateBuffer();
}

bool StreamBuffer::AllocateBuffer()
{
  // Create the buffer descriptor
  VkBufferCreateInfo buffer_create_info = {
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,  // VkStructureType        sType
      nullptr,                               // const void*            pNext
      0,                                     // VkBufferCreateFlags    flags
      static_cast<VkDeviceSize>(m_size),     // VkDeviceSize           size
      m_usage,                               // VkBufferUsageFlags     usage
      VK_SHARING_MODE_EXCLUSIVE,             // VkSharingMode          sharingMode
      0,                                     // uint32_t               queueFamilyIndexCount
      nullptr                                // const uint32_t*        pQueueFamilyIndices
  };

  // Host-visible system memory, mapped for the buffer's life: vertices,
  // indices, uniforms and texels are written straight into what the GPU reads.
  // Host visible VRAM is slower in practice.
  VkBuffer buffer = VK_NULL_HANDLE;
  vk_heap_alloc_t alloc;
  VkResult res = vk_heap_create_buffer(g_vulkan_context->GetHeap(), &buffer_create_info,
                                       m_heap_usage, &buffer, &alloc);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vk_heap_create_buffer failed: ");
    return false;
  }

  // Destroy the backings for the buffer after the command buffer executes
  if (m_buffer != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);

  // Replace with the new buffer
  m_buffer = buffer;
  m_alloc = alloc;
  m_host_pointer = static_cast<u8*>(alloc.mapped);
  m_current_offset = 0;
  m_current_gpu_position = 0;
  m_num_tracked = 0;
  return true;
}

bool StreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
  const u32 required_bytes = num_bytes + alignment;

  // Check for sane allocations
  if (required_bytes > m_size)
  {
    PanicAlertFmt("Attempting to allocate {} bytes from a {} byte stream buffer", num_bytes,
                  m_size);

    return false;
  }

  // Is the GPU behind or up to date with our current offset?
  UpdateCurrentFencePosition();
  if (m_current_offset >= m_current_gpu_position)
  {
    const u32 remaining_bytes = m_size - m_current_offset;
    if (required_bytes <= remaining_bytes)
    {
      // Place at the current position, after the GPU position.
      m_current_offset = Common::AlignUp(m_current_offset, alignment);
      m_last_allocation_size = num_bytes;
      return true;
    }

    // Check for space at the start of the buffer
    // We use < here because we don't want to have the case of m_current_offset ==
    // m_current_gpu_position. That would mean the code above would assume the
    // GPU has caught up to us, which it hasn't.
    if (required_bytes < m_current_gpu_position)
    {
      // Reset offset to zero, since we're allocating behind the gpu now
      m_current_offset = 0;
      m_last_allocation_size = num_bytes;
      return true;
    }
  }

  // Is the GPU ahead of our current offset?
  if (m_current_offset < m_current_gpu_position)
  {
    // We have from m_current_offset..m_current_gpu_position space to use.
    const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
    if (required_bytes < remaining_bytes)
    {
      // Place at the current position, since this is still behind the GPU.
      m_current_offset = Common::AlignUp(m_current_offset, alignment);
      m_last_allocation_size = num_bytes;
      return true;
    }
  }

  // Can we find a fence to wait on that will give us enough memory?
  if (WaitForClearSpace(required_bytes))
  {
    m_current_offset = Common::AlignUp(m_current_offset, alignment);
    m_last_allocation_size = num_bytes;
    return true;
  }

  // We tried everything we could, and still couldn't get anything. This means that too much space
  // in the buffer is being used by the command buffer currently being recorded. Therefore, the
  // only option is to execute it, and wait until it's done.
  return false;
}

void StreamBuffer::CommitMemory(u32 final_num_bytes)
{
  ASSERT((m_current_offset + final_num_bytes) <= m_size);
  ASSERT(final_num_bytes <= m_last_allocation_size);

  // For non-coherent mappings, flush the memory range; nothing to do on coherent memory
  vk_heap_flush(g_vulkan_context->GetHeap(), &m_alloc, m_current_offset, final_num_bytes);

  m_current_offset += final_num_bytes;
}

void StreamBuffer::UpdateCurrentFencePosition()
{
  // Don't create a tracking entry if the GPU is caught up with the buffer.
  if (m_current_offset == m_current_gpu_position)
    return;

  // Has the offset changed since the last fence?
  const u64 counter = g_command_buffer_mgr->GetCurrentFenceCounter();
  if (m_num_tracked && Tracked(m_num_tracked - 1).counter == counter)
  {
    // Still haven't executed a command buffer, so just update the offset.
    Tracked(m_num_tracked - 1).offset = m_current_offset;
    return;
  }

  // New buffer, so update the GPU position while we're at it.
  UpdateGPUPosition();
  if (m_num_tracked == TRACKED_FENCES)
  {
    // Never with fewer command buffers in flight than the ring holds; made room for all the same.
    g_command_buffer_mgr->WaitForFenceCounter(Tracked(0).counter);
    m_current_gpu_position = Tracked(0).offset;
    DropTracked(1);
  }
  TrackedFence& fence = Tracked(m_num_tracked++);
  fence.counter = counter;
  fence.offset = m_current_offset;
}

void StreamBuffer::UpdateGPUPosition()
{
  const u64 completed_counter = g_command_buffer_mgr->GetCompletedFenceCounter();
  u32 done = 0;
  while (done < m_num_tracked && completed_counter >= Tracked(done).counter)
  {
    m_current_gpu_position = Tracked(done).offset;
    done++;
  }
  DropTracked(done);
}

bool StreamBuffer::WaitForClearSpace(u32 num_bytes)
{
  u32 new_offset = 0;
  u32 new_gpu_position = 0;

  u32 i = 0;
  for (; i < m_num_tracked; i++)
  {
    // Would this fence bring us in line with the GPU?
    // This is the "last resort" case, where a command buffer execution has been forced
    // after no additional data has been written to it, so we can assume that after the
    // fence has been signaled the entire buffer is now consumed.
    u32 gpu_position = Tracked(i).offset;
    if (m_current_offset == gpu_position)
    {
      new_offset = 0;
      new_gpu_position = 0;
      break;
    }

    // Assuming that we wait for this fence, are we allocating in front of the GPU?
    if (m_current_offset > gpu_position)
    {
      // This would suggest the GPU has now followed us and wrapped around, so we have from
      // m_current_position..m_size free, as well as and 0..gpu_position.
      const u32 remaining_space_after_offset = m_size - m_current_offset;
      if (remaining_space_after_offset >= num_bytes)
      {
        // Switch to allocating in front of the GPU, using the remainder of the buffer.
        new_offset = m_current_offset;
        new_gpu_position = gpu_position;
        break;
      }

      // We can wrap around to the start, behind the GPU, if there is enough space.
      // We use > here because otherwise we'd end up lining up with the GPU, and then the
      // allocator would assume that the GPU has consumed what we just wrote.
      if (gpu_position > num_bytes)
      {
        new_offset = 0;
        new_gpu_position = gpu_position;
        break;
      }
    }
    else
    {
      // We're currently allocating behind the GPU. This would give us between the current
      // offset and the GPU position worth of space to work with. Again, > because we can't
      // align the GPU position with the buffer offset.
      u32 available_space_inbetween = gpu_position - m_current_offset;
      if (available_space_inbetween > num_bytes)
      {
        // Leave the offset as-is, but update the GPU position.
        new_offset = m_current_offset;
        new_gpu_position = gpu_position;
        break;
      }
    }
  }

  // Did any fences satisfy this condition?
  // Has the command buffer been executed yet? If not, the caller should execute it.
  if (i == m_num_tracked || Tracked(i).counter == g_command_buffer_mgr->GetCurrentFenceCounter())
  {
    return false;
  }

  // Wait until this fence is signaled. This will fire the callback, updating the GPU position.
  g_command_buffer_mgr->WaitForFenceCounter(Tracked(i).counter);
  DropTracked(m_current_offset == Tracked(i).offset ? m_num_tracked : i + 1);
  m_current_offset = new_offset;
  m_current_gpu_position = new_gpu_position;
  return true;
}

}  // namespace Vulkan
