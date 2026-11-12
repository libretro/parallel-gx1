
// must be first
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include <libretro_vulkan.h>
#include <stdlib.h>
#include <string.h>

#include "Core/Config/MainSettings.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/Vulkan.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/FramebufferManager.h"

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

/* A list of names, as Vulkan takes them, with room for every name it can be given. */
typedef struct
{
  const char** names;
  uint32_t count;
} name_list_t;

static bool HasName(const name_list_t* list, const char* value)
{
  uint32_t i;
  for (i = 0; i < list->count; i++)
  {
    if (!strcmp(value, list->names[i]))
      return true;
  }
  return false;
}

static void AddNameUnique(name_list_t* list, const char* value)
{
  if (!HasName(list, value))
    list->names[list->count++] = value;
}

static bool ExtensionAvailable(const VkExtensionProperties* props, uint32_t count,
                               const char* name)
{
  uint32_t i;
  for (i = 0; i < count; i++)
  {
    if (!strcmp(name, props[i].extensionName))
      return true;
  }
  return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                     const VkDeviceCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkDevice* pDevice)
{
  VkDeviceCreateInfo info = *pCreateInfo;
  const bool use_frontend_wrapper = initInfo.create_device_wrapper != nullptr;
  /* the backend's names, the frontend's, and the one more this adds */
  const uint32_t max_layers = info.enabledLayerCount + initInfo.num_required_device_layers;
  const uint32_t max_exts =
      info.enabledExtensionCount + initInfo.num_required_device_extensions + 1;
  VkPhysicalDeviceFeatures enabled_features{};
  VkExtensionProperties* props = nullptr;
  uint32_t ext_count = 0;
  name_list_t enabled_layers;
  name_list_t enabled_exts;
  name_list_t filtered_exts;
  VkResult result;
  uint32_t i;

  /* Freed before this returns: the lists only live through the create call. */
  enabled_layers.names = (const char**)malloc((max_layers + 1) * sizeof(const char*));
  enabled_exts.names = (const char**)malloc(max_exts * sizeof(const char*));
  filtered_exts.names = (const char**)malloc(max_exts * sizeof(const char*));
  enabled_layers.count = enabled_exts.count = filtered_exts.count = 0;
  if (!enabled_layers.names || !enabled_exts.names || !filtered_exts.names)
  {
    result = VK_ERROR_OUT_OF_HOST_MEMORY;
    goto done;
  }

  // Copy requested layers/extensions/features from the incoming create-info
  for (i = 0; i < info.enabledLayerCount; i++)
    enabled_layers.names[enabled_layers.count++] = info.ppEnabledLayerNames[i];
  for (i = 0; i < info.enabledExtensionCount; i++)
    enabled_exts.names[enabled_exts.count++] = info.ppEnabledExtensionNames[i];

  // Merge in libretro-required layers/extensions/features
  for (i = 0; i < initInfo.num_required_device_layers; i++)
    AddNameUnique(&enabled_layers, initInfo.required_device_layers[i]);

  for (i = 0; i < initInfo.num_required_device_extensions; i++)
    AddNameUnique(&enabled_exts, initInfo.required_device_extensions[i]);

  if (!use_frontend_wrapper)
    AddNameUnique(&enabled_exts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);

  // OR in required features bitwise (struct of VkBool32)
  if (info.pEnabledFeatures)
    enabled_features = *info.pEnabledFeatures;
  if (!use_frontend_wrapper && initInfo.required_features)
  {
    for (i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
    {
      if (reinterpret_cast<const VkBool32*>(initInfo.required_features)[i])
        reinterpret_cast<VkBool32*>(&enabled_features)[i] = VK_TRUE;
    }
  }

  // What this exact physicalDevice offers
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, nullptr);
  if (ext_count)
  {
    props = (VkExtensionProperties*)malloc(ext_count * sizeof(VkExtensionProperties));
    if (!props)
    {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto done;
    }
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, props);
  }

  // The v1 callback has no device-create wrapper, so filter extensions here.
  // With v2, preserve the backend's create-info for the frontend to augment.
  if (use_frontend_wrapper)
  {
    for (i = 0; i < enabled_exts.count; i++)
      filtered_exts.names[filtered_exts.count++] = enabled_exts.names[i];
    if (ExtensionAvailable(props, ext_count, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))
      AddNameUnique(&filtered_exts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
  }
  else
  {
    for (i = 0; i < enabled_exts.count; i++)
    {
      const char* name = enabled_exts.names[i];
      uint32_t j;
      bool seen = false;

      if (!name)
        continue;

      // De-dup while preserving order
      for (j = 0; j < i && !seen; j++)
        seen = enabled_exts.names[j] && !strcmp(name, enabled_exts.names[j]);
      if (seen)
        continue;

      if (ExtensionAvailable(props, ext_count, name))
        filtered_exts.names[filtered_exts.count++] = name;
      else
        WARN_LOG(VIDEO, "Dropping unsupported device extension: %s", name);
    }
  }

  // Finalize create-info with filtered lists
  info.enabledLayerCount = enabled_layers.count;
  info.ppEnabledLayerNames = info.enabledLayerCount ? enabled_layers.names : nullptr;

  info.enabledExtensionCount = filtered_exts.count;
  info.ppEnabledExtensionNames = info.enabledExtensionCount ? filtered_exts.names : nullptr;

  if (use_frontend_wrapper)
  {
    *pDevice = initInfo.create_device_wrapper(physicalDevice, initInfo.create_device_opaque, &info);
    result = *pDevice != VK_NULL_HANDLE ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  }
  else
  {
    info.pEnabledFeatures = &enabled_features;
    result = vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
  }

done:
  free(props);
  free(filtered_exts.names);
  free(enabled_exts.names);
  free(enabled_layers.names);
  return result;
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

  /* The frontend's extensions, and the one more the backend wants without a wrapper; kept
   * until the device is created, and replaced on the next context. */
  static const char** combined_extensions;
  const unsigned combined_count = num_required_device_extensions + (create_device_wrapper ? 0 : 1);
  unsigned i;

  free(combined_extensions);
  combined_extensions = (const char**)malloc((combined_count + 1) * sizeof(const char*));
  if (!combined_extensions)
  {
    initInfo.required_device_extensions = required_device_extensions;
    initInfo.num_required_device_extensions = num_required_device_extensions;
  }
  else
  {
    for (i = 0; i < num_required_device_extensions; ++i)
      combined_extensions[i] = required_device_extensions[i];
    if (!create_device_wrapper)
      combined_extensions[i] = "VK_KHR_get_physical_device_properties2";
    initInfo.required_device_extensions = combined_extensions;
    initInfo.num_required_device_extensions = combined_count;
  }

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
