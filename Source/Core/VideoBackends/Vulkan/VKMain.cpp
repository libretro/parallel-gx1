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

#if defined(VK_USE_PLATFORM_METAL_EXT)
#include <objc/message.h>
#endif

namespace Vulkan
{
void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  VulkanContext::PopulateBackendInfo(&g_backend_info);

  if (LoadVulkanLibrary())
  {
    u32 vk_api_version = 0;
    VkInstance temp_instance = VulkanContext::CreateVulkanInstance(WindowSystemType::Headless,
                                                                   false, false, &vk_api_version);
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
bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
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

void VideoBackend::PrepareWindow(WindowSystemInfo& wsi)
{
#if defined(VK_USE_PLATFORM_METAL_EXT)
  // We only need to manually create the CAMetalLayer on macOS.
  if (wsi.type != WindowSystemType::MacOS)
    return;

  // This is kinda messy, but it avoids having to write Objective C++ just to create a metal layer.
  id view = reinterpret_cast<id>(wsi.render_surface);
  Class clsCAMetalLayer = objc_getClass("CAMetalLayer");
  if (!clsCAMetalLayer)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to get CAMetalLayer class.");
    return;
  }

  // [CAMetalLayer layer]
  id layer = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("CAMetalLayer"),
                                                                sel_getUid("layer"));
  if (!layer)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create Metal layer.");
    return;
  }

  // [view setWantsLayer:YES]
  reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(view, sel_getUid("setWantsLayer:"), YES);

  // [view setLayer:layer]
  reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(view, sel_getUid("setLayer:"), layer);

  // NSScreen* screen = [NSScreen mainScreen]
  id screen = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("NSScreen"),
                                                                 sel_getUid("mainScreen"));

  // CGFloat factor = [screen backingScaleFactor]
  double factor =
      reinterpret_cast<double (*)(id, SEL)>(objc_msgSend)(screen, sel_getUid("backingScaleFactor"));

  // layer.contentsScale = factor
  reinterpret_cast<void (*)(id, SEL, double)>(objc_msgSend)(layer, sel_getUid("setContentsScale:"),
                                                            factor);

  // Store the layer pointer, that way MoltenVK doesn't call [NSView layer] outside the main thread.
  wsi.render_surface = layer;
#endif
}
}  // namespace Vulkan
