#pragma once

#include <libretro.h>
#include "DolphinLibretro/Common/Globals.h"
#include "DolphinLibretro/Common/Options.h"
#include "VideoBackends/Null/NullGfx.h"
#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWGfx.h"
#include "VideoBackends/Software/SWTexture.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "Common/Logging/Log.h"
#ifdef _WIN32
#define HAVE_D3D11
#define HAVE_D3D12
#include <libretro_d3d.h>
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#endif
#ifdef HAS_VULKAN
#ifndef __APPLE__
#include "VideoBackends/Vulkan/VulkanLoader.h"
#endif
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "DolphinLibretro/Vulkan.h"
#include <libretro_vulkan.h>
#endif

namespace Libretro
{
namespace Video
{
int GetAdjustedBaseWidth();
int GetAdjustedBaseHeight();
void Init(void);
bool Video_InitializeBackend();
bool SetHWRender(retro_hw_context_type type, const int version_major = -1, const int version_minor = -1);
void ContextReset(void);
void ContextDestroy(void);

/* Direct3D 11 version 2 shares the immediate context with the frontend
 * by taking turns: the core holds the lock around everything it does
 * with the context, and never across video_refresh or a return to the
 * frontend. Recursive; no-ops on other APIs and on version 1. */
void D3D11Lock(void);
void D3D11Unlock(void);
/* Releases every level held, for video_refresh or a wait on the
 * frontend; returns them for D3D11Resume. */
unsigned D3D11Suspend(void);
void D3D11Resume(unsigned depth);

/* At the end of each retro_run: what the GPU thread finished is handed
 * over; when nothing new was shown, the frontend is told the last picture
 * repeats, so every run reports one frame. */
void EndFrame();

/* The GPU loop on a thread of its own, for the APIs whose frontend
 * interface lets another thread submit (Vulkan): the frontend's thread
 * starts each field and takes what is done when its frame is due, and a
 * shader compiled or anything else slow on the GPU side holds up only
 * that thread. Pictures are published by the output and handed over by
 * EndFrame. */
bool GpuThreadWanted();
bool GpuThreadRunning();
void GpuThreadEnsure();
void GpuThreadStop();
/* Waits, until deadline_us of cpu_features_get_time_usec() or with 0 for
 * as long as it takes, for a field to end on the GPU thread since the
 * last call; true if one did. */
bool GpuThreadWaitField(int64_t deadline_us);
/* Stops the GPU thread between two runs of its loop, so the frontend's
 * thread may use the video state, and lets it go again. */
void GpuThreadPark();
void GpuThreadUnpark();

class SWGfx : public SW::SWGfx
{
public:
  SWGfx()
    : SW::SWGfx(SWOGLWindow::Create(
            WindowSystemInfo(WindowSystemType::Libretro, nullptr, nullptr, nullptr)))
  {
  }
  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc) override
  {
    SW::SWGfx::ShowImage(source_texture, source_rc);
    video_cb(
      static_cast<const SW::SWTexture*>(source_texture)->GetData(0, 0),
      source_rc.GetWidth(),
      source_rc.GetHeight(),
      source_texture->GetWidth() * 4
    );
    UpdateActiveConfig();
  }
};

class NullGfx : public Null::NullGfx
{
public:
  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc) override
  {
    video_cb(NULL, 512, 512, 512 * 4);
    UpdateActiveConfig();
  }
};

#ifdef HAS_VULKAN
namespace Vk
{
const VkApplicationInfo* GetApplicationInfo(void);

#ifdef __APPLE__
VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                          const VkApplicationInfo* app,
                          retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
                          void* opaque);
#endif
bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                  VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                  const char** required_device_extensions, unsigned num_required_device_extensions,
                  const char** required_device_layers, unsigned num_required_device_layers,
                  const VkPhysicalDeviceFeatures* required_features);
bool CreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                   VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                   retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque);
}  // namespace Vk
#endif

}  // namespace Video
}  // namespace Libretro
