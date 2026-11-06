#pragma once

#include <libretro_vulkan.h>

namespace Libretro
{
namespace Video
{
namespace Vk
{
void Init(VkInstance instance, VkPhysicalDevice gpu,
          PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
          unsigned num_required_device_extensions, const char** required_device_layers,
          unsigned num_required_device_layers, const VkPhysicalDeviceFeatures* required_features,
          retro_vulkan_create_device_wrapper_t create_device_wrapper = nullptr,
          void* opaque = nullptr);
void SetHWRenderInterface(retro_hw_render_interface* hw_render_interface);
retro_hw_render_interface_vulkan* GetHWRenderInterface();
void Shutdown();
}  // namespace Vk
}  // namespace Video
}  // namespace Libretro
