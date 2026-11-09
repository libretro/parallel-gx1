// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VideoBackend.h"

#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#include "VideoBackends/Vulkan/VKVertexManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
void VideoBackend::InitBackendInfo()
{
  VulkanContext::PopulateBackendInfo(&g_backend_info);

  if (LoadVulkanLibrary())
  {
    u32 vk_api_version = 0;
    VkInstance temp_instance = VulkanContext::CreateVulkanInstance(false, false, &vk_api_version);
    if (temp_instance)
    {
      if (LoadVulkanInstanceFunctions(temp_instance))
      {
        VulkanContext::GPUList gpu_list = VulkanContext::EnumerateGPUs(temp_instance);
        VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);

        if (!gpu_list.empty())
        {
          // Use the selected adapter, or the first to fill features.
          size_t device_index = static_cast<size_t>(g_Config.iAdapter);
          if (device_index >= gpu_list.size())
            device_index = 0;

          VkPhysicalDevice gpu = gpu_list[device_index];
          VulkanContext::PhysicalDeviceInfo properties(gpu);
          VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, gpu, properties);
          VulkanContext::PopulateBackendInfoMultisampleModes(&g_backend_info, gpu, properties);
        }
      }

      vkDestroyInstance(temp_instance, nullptr);
    }
    else
    {
      PanicAlertFmt("Failed to create Vulkan instance.");
    }

    UnloadVulkanLibrary();
  }
  else
  {
    PanicAlertFmt("Failed to load Vulkan library.");
  }
}

/* The frontend owns the instance, the device and the window:
 * Libretro::Video brings the backend up on the device it negotiated. */
bool VideoBackend::Initialize(float backbuffer_scale)
{
  return false;
}

void VideoBackend::Shutdown()
{
  if (g_vulkan_context)
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

  if (g_object_cache)
    g_object_cache->Shutdown();

  ShutdownShared();

  delete g_object_cache;
  g_object_cache = nullptr;
  StateTracker::DestroyInstance();
  delete g_command_buffer_mgr;
  g_command_buffer_mgr = nullptr;
  delete g_vulkan_context;
  g_vulkan_context = nullptr;
  UnloadVulkanLibrary();
}
}  // namespace Vulkan
