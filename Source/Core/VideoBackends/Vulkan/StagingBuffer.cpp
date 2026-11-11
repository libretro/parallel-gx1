// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/StagingBuffer.h"

#include <cstring>

#include "Common/Assert.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/DriverDetails.h"

namespace Vulkan
{
StagingBuffer::StagingBuffer(STAGING_BUFFER_TYPE type, VkBuffer buffer, const vk_heap_alloc_t& alloc,
                             VkDeviceSize size, char* map_ptr)
    : m_type(type), m_buffer(buffer), m_alloc(alloc), m_size(size), m_map_pointer(map_ptr)
{
}

StagingBuffer::~StagingBuffer()
{
  // Unmap before destroying
  if (m_map_pointer)
    Unmap();

  g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);
}

void StagingBuffer::BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer,
                                        VkAccessFlags src_access_mask,
                                        VkAccessFlags dst_access_mask, VkDeviceSize offset,
                                        VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
                                        VkPipelineStageFlags dst_stage_mask)
{
  VkBufferMemoryBarrier buffer_info = {
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,  // VkStructureType    sType
      nullptr,                                  // const void*        pNext
      src_access_mask,                          // VkAccessFlags      srcAccessMask
      dst_access_mask,                          // VkAccessFlags      dstAccessMask
      VK_QUEUE_FAMILY_IGNORED,                  // uint32_t           srcQueueFamilyIndex
      VK_QUEUE_FAMILY_IGNORED,                  // uint32_t           dstQueueFamilyIndex
      buffer,                                   // VkBuffer           buffer
      offset,                                   // VkDeviceSize       offset
      size                                      // VkDeviceSize       size
  };

  vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1,
                       &buffer_info, 0, nullptr);
}

bool StagingBuffer::Map()
{
  // The staging buffer's memory is mapped for as long as it lives
  return true;
}

void StagingBuffer::Unmap()
{
}

void StagingBuffer::FlushCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  // nothing to do on coherent memory
  vk_heap_flush(g_vulkan_context->GetHeap(), &m_alloc, offset, size);
}

void StagingBuffer::InvalidateGPUCache(VkCommandBuffer command_buffer,
                                       VkAccessFlagBits dest_access_flags,
                                       VkPipelineStageFlagBits dest_pipeline_stage,
                                       VkDeviceSize offset, VkDeviceSize size)
{
  ASSERT((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  BufferMemoryBarrier(command_buffer, m_buffer, VK_ACCESS_HOST_WRITE_BIT, dest_access_flags, offset,
                      size, VK_PIPELINE_STAGE_HOST_BIT, dest_pipeline_stage);
}

void StagingBuffer::PrepareForGPUWrite(VkCommandBuffer command_buffer,
                                       VkAccessFlagBits dst_access_flags,
                                       VkPipelineStageFlagBits dst_pipeline_stage,
                                       VkDeviceSize offset, VkDeviceSize size)
{
  ASSERT((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  BufferMemoryBarrier(command_buffer, m_buffer, VK_ACCESS_MEMORY_WRITE_BIT, dst_access_flags,
                      offset, size, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_pipeline_stage);
}

void StagingBuffer::FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
                                  VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset,
                                  VkDeviceSize size)
{
  ASSERT((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  BufferMemoryBarrier(command_buffer, m_buffer, src_access_flags, VK_ACCESS_HOST_READ_BIT, offset,
                      size, src_pipeline_stage, VK_PIPELINE_STAGE_HOST_BIT);
}

void StagingBuffer::InvalidateCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  // nothing to do on coherent memory
  vk_heap_invalidate(g_vulkan_context->GetHeap(), &m_alloc, offset, size);
}

void StagingBuffer::Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches)
{
  ASSERT((offset + size) <= m_size);
  if (invalidate_caches)
    InvalidateCPUCache(offset, size);

  memcpy(data, m_map_pointer + offset, size);
}

void StagingBuffer::Write(VkDeviceSize offset, const void* data, size_t size,
                          bool invalidate_caches)
{
  ASSERT((offset + size) <= m_size);

  memcpy(m_map_pointer + offset, data, size);
  if (invalidate_caches)
    FlushCPUCache(offset, size);
}

bool StagingBuffer::AllocateBuffer(STAGING_BUFFER_TYPE type, VkDeviceSize size,
                                   VkBufferUsageFlags usage, VkBuffer* out_buffer,
                                   vk_heap_alloc_t* out_alloc, char** out_map_ptr)
{
  VkBufferCreateInfo buffer_create_info = {
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,  // VkStructureType        sType
      nullptr,                               // const void*            pNext
      0,                                     // VkBufferCreateFlags    flags
      size,                                  // VkDeviceSize           size
      usage,                                 // VkBufferUsageFlags     usage
      VK_SHARING_MODE_EXCLUSIVE,             // VkSharingMode          sharingMode
      0,                                     // uint32_t               queueFamilyIndexCount
      nullptr                                // const uint32_t*        pQueueFamilyIndices
  };

  // Where cached memory is slow to read back, coherent memory is taken
  // whatever else it is, cached if there is such.
  vk_heap_usage heap_usage = type == STAGING_BUFFER_TYPE_UPLOAD ? VK_HEAP_UPLOAD : VK_HEAP_READBACK;
  if (DriverDetails::HasBug(DriverDetails::BUG_SLOW_CACHED_READBACK_MEMORY)) [[unlikely]]
    heap_usage = VK_HEAP_HOST_COHERENT;

  VkResult res = vk_heap_create_buffer(g_vulkan_context->GetHeap(), &buffer_create_info,
                                       heap_usage, out_buffer, out_alloc);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vk_heap_create_buffer failed: ");
    *out_map_ptr = nullptr;
    return false;
  }

  if (type == STAGING_BUFFER_TYPE_UPLOAD &&
      !(vk_heap_flags(g_vulkan_context->GetHeap(), out_alloc) &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
  {
    WARN_LOG(VIDEO, "Vulkan: Failed to find a coherent memory type for uploads, this will "
                    "affect performance.");
  }

  *out_map_ptr = static_cast<char*>(out_alloc->mapped);
  return true;
}

StagingBuffer* StagingBuffer::Create(STAGING_BUFFER_TYPE type, VkDeviceSize size,
                                     VkBufferUsageFlags usage)
{
  VkBuffer buffer;
  vk_heap_alloc_t alloc;
  char* map_ptr;
  if (!AllocateBuffer(type, size, usage, &buffer, &alloc, &map_ptr))
    return nullptr;

  return new StagingBuffer(type, buffer, alloc, size, map_ptr);
}

}  // namespace Vulkan
