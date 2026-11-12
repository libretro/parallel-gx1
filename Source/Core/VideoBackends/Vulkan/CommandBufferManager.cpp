// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/CommandBufferManager.h"

#include <cstdint>
#include <cstdlib>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"
#include "vulkan/vulkan_core.h"

namespace Vulkan
{
CommandBufferManager::CommandBufferManager(bool use_threaded_submission)
    : m_use_threaded_submission(use_threaded_submission)
{
}

CommandBufferManager::~CommandBufferManager()
{
  // If the worker thread is enabled, stop and block until it exits.
  if (m_use_threaded_submission)
  {
    WaitForWorkerThreadIdle();
    m_submit_thread.Shutdown();
  }

  DestroyCommandBuffers();
}

bool CommandBufferManager::Initialize()
{
  if (!CreateCommandBuffers())
    return false;

  if (m_use_threaded_submission && !CreateSubmitThread())
    return false;

  return true;
}

static bool Reserve(void** data, u32 count, u32* capacity, size_t size)
{
  void* grown;
  u32 n;
  if (count < *capacity)
    return true;
  n = *capacity ? *capacity * 2 : 16;
  grown = realloc(*data, static_cast<size_t>(n) * size);
  if (!grown)
    return false;
  *data = grown;
  *capacity = n;
  return true;
}

bool CommandBufferManager::CreateCommandBuffers()
{
  VkDevice device = g_vulkan_context->GetDevice();
  VkResult res;

  for (CmdBufferResources& resources : m_command_buffers)
  {
    resources.init_command_buffer_used = false;

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0,
                                         g_vulkan_context->GetGraphicsQueueFamilyIndex()};
    res = vkCreateCommandPool(g_vulkan_context->GetDevice(), &pool_info, nullptr,
                              &resources.command_pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
      return false;
    }

    VkCommandBufferAllocateInfo buffer_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, resources.command_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};

    res = vkAllocateCommandBuffers(device, &buffer_info, resources.command_buffers);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
      return false;
    }

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                    VK_FENCE_CREATE_SIGNALED_BIT};

    res = vkCreateFence(device, &fence_info, nullptr, &resources.fence);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
      return false;
    }
  }

  /* the lists' first room, now: a frame's first descriptor pool and cleanups allocate nothing */
  for (FrameResources& frame : m_frame_resources)
  {
    if (!Reserve(reinterpret_cast<void**>(&frame.descriptor_pools), 0,
                 &frame.descriptor_pools_capacity, sizeof(VkDescriptorPool)))
      return false;
  }
  for (CmdBufferResources& resources : m_command_buffers)
  {
    if (!Reserve(reinterpret_cast<void**>(&resources.cleanup), 0, &resources.cleanup_capacity,
                 sizeof(DeferredDestruction)))
      return false;
  }

  // Activate the first command buffer. BeginCommandBuffer moves forward, so start with the last
  m_current_cmd_buffer = NUM_COMMAND_BUFFERS - 1;
  BeginCommandBuffer();
  return true;
}

void CommandBufferManager::DestroyCommandBuffers()
{
  VkDevice device = g_vulkan_context->GetDevice();

  for (CmdBufferResources& resources : m_command_buffers)
  {
    // The Vulkan spec section 5.2 says: "When a pool is destroyed, all command buffers allocated
    // from the pool are freed.". So we don't need to free the command buffers, just the pools.
    // We destroy the command pool first, to avoid any warnings from the validation layers about
    // objects which are pending destruction being in-use.
    if (resources.command_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, resources.command_pool, nullptr);

    // Destroy any pending objects.
    for (u32 i = 0; i < resources.num_cleanup; i++)
      Destroy(&resources.cleanup[i]);
    free(resources.cleanup);
    resources.cleanup = nullptr;
    resources.num_cleanup = resources.cleanup_capacity = 0;

    if (resources.fence != VK_NULL_HANDLE)
      vkDestroyFence(device, resources.fence, nullptr);
  }

  for (FrameResources& resources : m_frame_resources)
  {
    for (u32 i = 0; i < resources.num_descriptor_pools; i++)
      vkDestroyDescriptorPool(device, resources.descriptor_pools[i], nullptr);
    free(resources.descriptor_pools);
    resources.descriptor_pools = nullptr;
    resources.num_descriptor_pools = resources.descriptor_pools_capacity = 0;
  }
}

/* Room for one more of size bytes in a list grown by doubling: false where none was left. */
VkDescriptorPool CommandBufferManager::CreateDescriptorPool(u32 max_descriptor_sets)
{
  /*
   * Worst case descriptor counts according to the descriptor layout created in ObjectCache.cpp:
   * UNIFORM_BUFFER_DYNAMIC: 3
   * COMBINED_IMAGE_SAMPLER: NUM_UTILITY_PIXEL_SAMPLERS + NUM_COMPUTE_SHADER_SAMPLERS +
   * VideoCommon::MAX_PIXEL_SHADER_SAMPLERS
   * STORAGE_BUFFER: 2
   * UNIFORM_TEXEL_BUFFER: 3
   * STORAGE_IMAGE: 1
   */
  const VkDescriptorPoolSize pool_sizes[5] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, max_descriptor_sets * 3},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       max_descriptor_sets *
           (VideoCommon::MAX_PIXEL_SHADER_SAMPLERS + VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS +
            NUM_UTILITY_PIXEL_SAMPLERS)},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_descriptor_sets * 2},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, max_descriptor_sets * 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_descriptor_sets * 1},
  };

  const VkDescriptorPoolCreateInfo pool_create_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,           0, max_descriptor_sets,
      5,                                             pool_sizes,
  };

  VkDevice device = g_vulkan_context->GetDevice();
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkResult res = vkCreateDescriptorPool(device, &pool_create_info, nullptr, &descriptor_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
    return VK_NULL_HANDLE;
  }
  return descriptor_pool;
}

VkDescriptorSet CommandBufferManager::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
{
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  FrameResources& resources = GetCurrentFrameResources();

  if (resources.num_descriptor_pools) [[likely]]
  {
    VkDescriptorSetAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
        resources.descriptor_pools[resources.current_descriptor_pool_index], 1, &set_layout};

    VkResult res =
        vkAllocateDescriptorSets(g_vulkan_context->GetDevice(), &allocate_info, &descriptor_set);
    if (res != VK_SUCCESS &&
        resources.num_descriptor_pools > resources.current_descriptor_pool_index + 1)
    {
      // Mark the next descriptor set as active and try again.
      resources.current_descriptor_pool_index++;
      descriptor_set = AllocateDescriptorSet(set_layout);
    }
  }

  if (descriptor_set == VK_NULL_HANDLE) [[unlikely]]
  {
    VkDescriptorPool descriptor_pool = CreateDescriptorPool(DESCRIPTOR_SETS_PER_POOL);
    m_descriptor_set_count += DESCRIPTOR_SETS_PER_POOL;
    if (descriptor_pool == VK_NULL_HANDLE) [[unlikely]]
      return VK_NULL_HANDLE;

    if (!Reserve(reinterpret_cast<void**>(&resources.descriptor_pools),
                 resources.num_descriptor_pools, &resources.descriptor_pools_capacity,
                 sizeof(VkDescriptorPool))) [[unlikely]]
    {
      vkDestroyDescriptorPool(g_vulkan_context->GetDevice(), descriptor_pool, nullptr);
      return VK_NULL_HANDLE;
    }
    resources.descriptor_pools[resources.num_descriptor_pools++] = descriptor_pool;
    resources.current_descriptor_pool_index = resources.num_descriptor_pools - 1;
    descriptor_set = AllocateDescriptorSet(set_layout);
  }

  return descriptor_set;
}

bool CommandBufferManager::CreateSubmitThread()
{
  m_submit_thread.Reset("VK submission thread", [this](PendingCommandBufferSubmit submit) {
    SubmitCommandBuffer(submit.command_buffer_index);
    CmdBufferResources& resources = m_command_buffers[submit.command_buffer_index];
    retro_atomic_store_release_int(&resources.waiting_for_submit, 0);
  });

  return true;
}

void CommandBufferManager::WaitForWorkerThreadIdle()
{
  if (!m_use_threaded_submission)
    return;

  m_submit_thread.WaitForCompletion();
}

void CommandBufferManager::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  // Find the first command buffer which covers this counter value.
  u32 index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_cmd_buffer)
  {
    if (m_command_buffers[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }

  ASSERT(index != m_current_cmd_buffer);
  WaitForCommandBufferCompletion(index);
}

bool CommandBufferManager::PollFenceCounter(u64 fence_counter)
{
  u32 index;

  if (m_completed_fence_counter >= fence_counter)
    return true;

  // The first command buffer which covers this counter value, if submitted.
  index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_cmd_buffer)
  {
    if (m_command_buffers[index].fence_counter >= fence_counter)
      break;
    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }
  if (index == m_current_cmd_buffer ||
      retro_atomic_load_acquire_int(&m_command_buffers[index].waiting_for_submit))
  {
    return false;
  }
  return vkGetFenceStatus(g_vulkan_context->GetDevice(), m_command_buffers[index].fence) ==
         VK_SUCCESS;
}

bool FenceCounterDone(u64 fence_counter)
{
  return g_command_buffer_mgr->PollFenceCounter(fence_counter);
}

void CommandBufferManager::WaitForCommandBufferCompletion(u32 index)
{
  CmdBufferResources& resources = m_command_buffers[index];

  // Ensure this command buffer has been submitted.
  if (retro_atomic_load_acquire_int(&resources.waiting_for_submit))
  {
    WaitForWorkerThreadIdle();
    ASSERT_MSG(VIDEO, !retro_atomic_load_relaxed_int(&resources.waiting_for_submit),
               "Submit thread is idle but command buffer is still waiting for submission!");
  }

  // Wait for this command buffer to be completed.
  VkResult res =
      vkWaitForFences(g_vulkan_context->GetDevice(), 1, &resources.fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");

  // Clean up any resources for command buffers between the last known completed buffer and this
  // now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
  const u64 now_completed_counter = resources.fence_counter;
  u32 cleanup_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  while (cleanup_index != m_current_cmd_buffer)
  {
    CmdBufferResources& cleanup_resources = m_command_buffers[cleanup_index];
    if (cleanup_resources.fence_counter > now_completed_counter)
      break;

    if (cleanup_resources.fence_counter > m_completed_fence_counter)
    {
      for (u32 i = 0; i < cleanup_resources.num_cleanup; i++)
        Destroy(&cleanup_resources.cleanup[i]);
      cleanup_resources.num_cleanup = 0;
    }

    cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
  }

  m_completed_fence_counter = now_completed_counter;
}

void CommandBufferManager::SubmitCommandBuffer(bool submit_on_worker_thread,
                                               bool wait_for_completion, bool advance_to_next_frame)
{
  // End the current command buffer.
  CmdBufferResources& resources = GetCurrentCmdBufferResources();
  if (m_before_submit)
    m_before_submit(m_before_submit_opaque, GetCurrentCommandBuffer());
  for (VkCommandBuffer command_buffer : resources.command_buffers)
  {
    VkResult res = vkEndCommandBuffer(command_buffer);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
      PanicAlert("Failed to end command buffer: %s (%d)", VkResultToString(res),
                 static_cast<int>(res));
    }
  }

  // Submitting off-thread?
  if (m_use_threaded_submission && submit_on_worker_thread && !wait_for_completion)
  {
    retro_atomic_store_relaxed_int(&resources.waiting_for_submit, 1);
    // Push to the pending submit queue.
    m_submit_thread.Push({m_current_cmd_buffer});
  }
  else
  {
    WaitForWorkerThreadIdle();

    // Pass through to normal submission path.
    SubmitCommandBuffer(m_current_cmd_buffer);
    if (wait_for_completion)
      WaitForCommandBufferCompletion(m_current_cmd_buffer);
  }

  if (advance_to_next_frame)
  {
    m_current_frame = (m_current_frame + 1) % NUM_FRAMES_IN_FLIGHT;

    // Wait for all command buffers that used the descriptor pool to finish
    u32 cmd_buffer_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
    while (cmd_buffer_index != m_current_cmd_buffer)
    {
      CmdBufferResources& cmd_buffer = m_command_buffers[cmd_buffer_index];
      if (cmd_buffer.frame_index == m_current_frame && cmd_buffer.fence_counter != 0 &&
          cmd_buffer.fence_counter > m_completed_fence_counter)
      {
        WaitForCommandBufferCompletion(cmd_buffer_index);
      }
      cmd_buffer_index = (cmd_buffer_index + 1) % NUM_COMMAND_BUFFERS;
    }

    // Reset the descriptor pools
    FrameResources& frame_resources = GetCurrentFrameResources();

    if (frame_resources.num_descriptor_pools == 1) [[likely]]
    {
      VkResult res = vkResetDescriptorPool(g_vulkan_context->GetDevice(),
                                           frame_resources.descriptor_pools[0], 0);
      if (res != VK_SUCCESS)
        LOG_VULKAN_ERROR(res, "vkResetDescriptorPool failed: ");
    }
    else [[unlikely]]
    {
      /* one pool for them all, where the list already has room */
      for (u32 i = 0; i < frame_resources.num_descriptor_pools; i++)
        vkDestroyDescriptorPool(g_vulkan_context->GetDevice(),
                                frame_resources.descriptor_pools[i], nullptr);
      frame_resources.num_descriptor_pools = 0;
      VkDescriptorPool descriptor_pool = CreateDescriptorPool(m_descriptor_set_count);
      if (descriptor_pool != VK_NULL_HANDLE &&
          Reserve(reinterpret_cast<void**>(&frame_resources.descriptor_pools), 0,
                  &frame_resources.descriptor_pools_capacity, sizeof(VkDescriptorPool)))
        [[likely]]
        frame_resources.descriptor_pools[frame_resources.num_descriptor_pools++] =
            descriptor_pool;
      else if (descriptor_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(g_vulkan_context->GetDevice(), descriptor_pool, nullptr);
    }

    frame_resources.current_descriptor_pool_index = 0;
  }

  // Switch to next cmdbuffer.
  BeginCommandBuffer();
}

void CommandBufferManager::SubmitCommandBuffer(u32 command_buffer_index)
{
  CmdBufferResources& resources = m_command_buffers[command_buffer_index];

  // This may be executed on the worker thread, so don't modify any state of the manager class.
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              nullptr,
                              0,
                              nullptr,
                              nullptr,
                              2,
                              resources.command_buffers,
                              0,
                              nullptr};

  // If the init command buffer did not have any commands recorded, don't submit it.
  if (!resources.init_command_buffer_used)
  {
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &resources.command_buffers[1];
  }

  VkResult res =
      vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 1, &submit_info, resources.fence);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
    PanicAlert("Failed to submit command buffer: %s (%d)", VkResultToString(res),
               static_cast<int>(res));
  }
}

void CommandBufferManager::BeginCommandBuffer()
{
  // Move to the next command buffer.
  const u32 next_buffer_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  CmdBufferResources& resources = m_command_buffers[next_buffer_index];

  // Wait for the GPU to finish with all resources for this command buffer.
  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(next_buffer_index);

  // Reset fence to unsignaled before starting.
  VkResult res = vkResetFences(g_vulkan_context->GetDevice(), 1, &resources.fence);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

  // Reset command pools to beginning since we can re-use the memory now
  res = vkResetCommandPool(g_vulkan_context->GetDevice(), resources.command_pool, 0);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

  // Enable commands to be recorded to the two buffers again.
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  for (VkCommandBuffer command_buffer : resources.command_buffers)
  {
    res = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");
  }

  // Reset upload command buffer state
  resources.init_command_buffer_used = false;
  resources.fence_counter = m_next_fence_counter++;
  resources.frame_index = m_current_frame;
  m_current_cmd_buffer = next_buffer_index;
}

void CommandBufferManager::Destroy(DeferredDestruction* d)
{
  const VkDevice device = g_vulkan_context->GetDevice();
  switch (d->kind)
  {
  case DeferredDestruction::BUFFER_VIEW:
    vkDestroyBufferView(device, d->buffer_view, nullptr);
    break;
  case DeferredDestruction::BUFFER:
    vk_heap_destroy_buffer(g_vulkan_context->GetHeap(), d->buffer, &d->alloc);
    break;
  case DeferredDestruction::FRAMEBUFFER:
    vkDestroyFramebuffer(device, d->framebuffer, nullptr);
    break;
  case DeferredDestruction::IMAGE:
    vk_heap_destroy_image(g_vulkan_context->GetHeap(), d->image, &d->alloc);
    break;
  case DeferredDestruction::IMAGE_VIEW:
    vkDestroyImageView(device, d->image_view, nullptr);
    break;
  }
}

/* Where the list cannot grow, the object is destroyed once the GPU is idle. */
void CommandBufferManager::Defer(const DeferredDestruction& d)
{
  CmdBufferResources& resources = GetCurrentCmdBufferResources();
  if (!Reserve(reinterpret_cast<void**>(&resources.cleanup), resources.num_cleanup,
               &resources.cleanup_capacity, sizeof(DeferredDestruction))) [[unlikely]]
  {
    DeferredDestruction now = d;
    WaitForWorkerThreadIdle();
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());
    Destroy(&now);
    return;
  }
  resources.cleanup[resources.num_cleanup++] = d;
}

void CommandBufferManager::DeferBufferDestruction(VkBuffer buffer, const vk_heap_alloc_t& alloc)
{
  DeferredDestruction d = {};
  d.kind = DeferredDestruction::BUFFER;
  d.buffer = buffer;
  d.alloc = alloc;
  Defer(d);
}

void CommandBufferManager::DeferFramebufferDestruction(VkFramebuffer object)
{
  DeferredDestruction d = {};
  d.kind = DeferredDestruction::FRAMEBUFFER;
  d.framebuffer = object;
  Defer(d);
}

void CommandBufferManager::DeferImageDestruction(VkImage image, const vk_heap_alloc_t& alloc)
{
  DeferredDestruction d = {};
  d.kind = DeferredDestruction::IMAGE;
  d.image = image;
  d.alloc = alloc;
  Defer(d);
}

void CommandBufferManager::DeferImageViewDestruction(VkImageView object)
{
  DeferredDestruction d = {};
  d.kind = DeferredDestruction::IMAGE_VIEW;
  d.image_view = object;
  Defer(d);
}

CommandBufferManager* g_command_buffer_mgr = nullptr;
}  // namespace Vulkan
