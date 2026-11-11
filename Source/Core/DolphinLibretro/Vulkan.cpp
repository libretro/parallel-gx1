
// must be first
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include <cstring>
#include <libretro_vulkan.h>
#include <vector>

#include "Core/Config/MainSettings.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/Vulkan.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/FramebufferManager.h"

#include <unordered_set>
#include <string>

/* The instance, device and surface are the frontend's, and so is the
 * queue, which the frontend submits to from its own thread. */
#define LIBRETRO_VK_WARP_LIST()                                                                    \
  LIBRETRO_VK_WARP_FUNC(vkDestroyInstance);                                                        \
  LIBRETRO_VK_WARP_FUNC(vkCreateDevice);                                                           \
  LIBRETRO_VK_WARP_FUNC(vkDestroyDevice);                                                          \
  LIBRETRO_VK_WARP_FUNC(vkDestroySurfaceKHR);                                                      \
  LIBRETRO_VK_WARP_FUNC(vkQueueSubmit);                                                            \
  LIBRETRO_VK_WARP_FUNC(vkQueueWaitIdle)

#define LIBRETRO_VK_WARP_FUNC(x)                                                                   \
  extern PFN_##x x;                                                                                \
  static PFN_##x x##_org

LIBRETRO_VK_WARP_FUNC(vkGetInstanceProcAddr);
LIBRETRO_VK_WARP_FUNC(vkGetDeviceProcAddr);
LIBRETRO_VK_WARP_LIST();

extern PFN_vkCreateInstance vkCreateInstance;

namespace Libretro
{
namespace Video
{
namespace Vk
{
static retro_hw_render_interface_vulkan* vulkan;

static struct
{
  VkInstance instance;
  VkPhysicalDevice gpu;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  const char** required_device_extensions;
  unsigned num_required_device_extensions;
  const char** required_device_layers;
  unsigned num_required_device_layers;
  const VkPhysicalDeviceFeatures* required_features;
  retro_vulkan_create_device_wrapper_t create_device_wrapper;
  void* create_device_opaque;
} initInfo;

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkInstance* pInstance)
{
  *pInstance = initInfo.instance;
  return VK_SUCCESS;
}

static void AddNameUnique(std::vector<const char*>& list, const char* value)
{
  for (const char* name : list)
    if (!strcmp(value, name))
      return;

  list.push_back(value);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                     const VkDeviceCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkDevice* pDevice)
{
  VkDeviceCreateInfo info = *pCreateInfo;

  // Copy requested layers/extensions/features from the incoming create-info
  std::vector<const char*> enabled_layers;
  if (info.enabledLayerCount)
    enabled_layers.assign(info.ppEnabledLayerNames,
                          info.ppEnabledLayerNames + info.enabledLayerCount);

  std::vector<const char*> enabled_exts;
  if (info.enabledExtensionCount)
    enabled_exts.assign(info.ppEnabledExtensionNames,
                        info.ppEnabledExtensionNames + info.enabledExtensionCount);

  // Merge in libretro-required layers/extensions/features
  for (unsigned i = 0; i < initInfo.num_required_device_layers; i++)
    AddNameUnique(enabled_layers, initInfo.required_device_layers[i]);

  for (unsigned i = 0; i < initInfo.num_required_device_extensions; i++)
    AddNameUnique(enabled_exts, initInfo.required_device_extensions[i]);

  const bool use_frontend_wrapper = initInfo.create_device_wrapper != nullptr;
  if (!use_frontend_wrapper)
    AddNameUnique(enabled_exts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);

  // OR in required features bitwise (struct of VkBool32)
  VkPhysicalDeviceFeatures enabled_features{};
  if (info.pEnabledFeatures)
    enabled_features = *info.pEnabledFeatures;
  if (!use_frontend_wrapper && initInfo.required_features)
  {
    for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
    {
      if (reinterpret_cast<const VkBool32*>(initInfo.required_features)[i])
        reinterpret_cast<VkBool32*>(&enabled_features)[i] = VK_TRUE;
    }
  }

  // Build availability set for this exact physicalDevice
  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> props(ext_count);
  if (ext_count)
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, props.data());

  std::unordered_set<std::string> avail;
  avail.reserve(props.size());
  for (const auto& p : props)
    avail.emplace(p.extensionName);

  // The v1 callback has no device-create wrapper, so filter extensions here.
  // With v2, preserve the backend's create-info for the frontend to augment.
  std::vector<const char*> filtered_exts;
  if (use_frontend_wrapper)
  {
    filtered_exts = enabled_exts;
    if (avail.find(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) != avail.end())
      AddNameUnique(filtered_exts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
  }
  else
  {
    filtered_exts.reserve(enabled_exts.size());

    std::unordered_set<std::string> seen;
    seen.reserve(enabled_exts.size());

    for (const char* name : enabled_exts)
    {
      if (!name)
        continue;

      // De-dup while preserving order
      if (!seen.insert(name).second)
        continue;

      if (avail.find(name) != avail.end())
      {
        filtered_exts.push_back(name);
      }
      else
      {
        WARN_LOG(VIDEO, "Dropping unsupported device extension: %s", name);
      }
    }
  }

  // Finalize create-info with filtered lists
  info.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
  info.ppEnabledLayerNames = info.enabledLayerCount ? enabled_layers.data() : nullptr;

  info.enabledExtensionCount = static_cast<uint32_t>(filtered_exts.size());
  info.ppEnabledExtensionNames = info.enabledExtensionCount ? filtered_exts.data() : nullptr;

  if (!use_frontend_wrapper)
    info.pEnabledFeatures = &enabled_features;

  if (use_frontend_wrapper)
  {
    *pDevice = initInfo.create_device_wrapper(physicalDevice, initInfo.create_device_opaque, &info);
    return *pDevice != VK_NULL_HANDLE ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  }

  return vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
}

static VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,
                                                    const VkAllocationCallbacks* pAllocator)
{
}

static VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device,
                                                  const VkAllocationCallbacks* pAllocator)
{
}

static VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                      const VkAllocationCallbacks* pAllocator)
{
}

static VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                    const VkSubmitInfo* pSubmits, VkFence fence)
{
  VkResult res;

  vulkan->lock_queue(vulkan->handle);
  res = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
  vulkan->unlock_queue(vulkan->handle);
  return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
  vulkan->lock_queue(vulkan->handle);
  VkResult res = vkQueueWaitIdle_org(queue);
  vulkan->unlock_queue(vulkan->handle);
  return res;
}

#undef LIBRETRO_VK_WARP_FUNC
#define LIBRETRO_VK_WARP_FUNC(x)                                                                   \
  do                                                                                               \
  {                                                                                                \
    if (!strcmp(pName, #x))                                                                        \
    {                                                                                              \
      x##_org = (PFN_##x)fptr;                                                                     \
      return (PFN_vkVoidFunction)Libretro::Video::Vk::x;                                           \
    }                                                                                              \
  } while (0)

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                      const char* pName)
{
  PFN_vkVoidFunction fptr = vkGetInstanceProcAddr_org(instance, pName);
  if (!fptr)
    return fptr;

  LIBRETRO_VK_WARP_LIST();

  return fptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                    const char* pName)
{
  PFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);
  if (!fptr)
    return fptr;

  LIBRETRO_VK_WARP_LIST();

  return fptr;
}

void Init(VkInstance instance, VkPhysicalDevice gpu,
          PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
          unsigned num_required_device_extensions, const char** required_device_layers,
          unsigned num_required_device_layers, const VkPhysicalDeviceFeatures* required_features,
          retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
  initInfo.instance = instance;
  initInfo.gpu = gpu;
  initInfo.get_instance_proc_addr = get_instance_proc_addr;

  static const char* extra_extensions[] = {"VK_KHR_get_physical_device_properties2"};
  const size_t extra_extension_count = create_device_wrapper ? 0 : std::size(extra_extensions);

  static std::vector<const char*> combined_extensions;
  combined_extensions.clear();
  combined_extensions.reserve(num_required_device_extensions + extra_extension_count);

  for (unsigned i = 0; i < num_required_device_extensions; ++i)
    combined_extensions.push_back(required_device_extensions[i]);
  for (size_t i = 0; i < extra_extension_count; ++i)
    combined_extensions.push_back(extra_extensions[i]);

  initInfo.required_device_extensions = combined_extensions.data();
  initInfo.num_required_device_extensions = static_cast<unsigned>(combined_extensions.size());

  initInfo.required_device_layers = required_device_layers;
  initInfo.num_required_device_layers = num_required_device_layers;
  initInfo.required_features = required_features;
  initInfo.create_device_wrapper = create_device_wrapper;
  initInfo.create_device_opaque = opaque;

  vkGetInstanceProcAddr_org = get_instance_proc_addr;
  ::vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  vkGetDeviceProcAddr_org =
    (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr_org(instance, "vkGetDeviceProcAddr");
  ::vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  ::vkCreateInstance = vkCreateInstance;
}

void SetHWRenderInterface(retro_hw_render_interface* hw_render_interface)
{
  vulkan = (retro_hw_render_interface_vulkan*)hw_render_interface;
}

retro_hw_render_interface_vulkan* GetHWRenderInterface()
{
  return vulkan;
}

void Shutdown()
{
  memset(&initInfo, 0x00, sizeof(initInfo));
  vulkan = NULL;
}
}  // namespace Vk
}  // namespace Video
}  // namespace Libretro
