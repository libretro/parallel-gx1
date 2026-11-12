// needs to be first
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VulkanLoader.h"
#endif
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/Output.h"

#include <cassert>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <vector>

#ifdef _WIN32
#include "VideoBackends/D3D/VideoBackend.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3D/D3DPerfQuery.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/VideoBackend.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/D3D12VertexManager.h"
#include "VideoBackends/D3D12/D3D12PerfQuery.h"
#include "VideoBackends/D3D12/D3D12BoundingBox.h"
#endif

#ifdef HAS_OPENGL
#include "Common/GL/GLContext.h"
#include "Common/GL/GLUtil.h"
#include "VideoBackends/OGL/OGLGfx.h"
#endif

#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/VR.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Present.h"

#include <features/features_cpu.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKVertexManager.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#endif

namespace Libretro
{
extern std::vector<std::string> g_normalized_game_paths;
namespace Video
{
/* The GPU thread (see GpuThreadMain). Its loop runs the GPU until the CPU
 * thread ends a field, which it counts, or a request to park or quit
 * interrupts it; idle, it parks with no timeout. */
static sthread_t* s_gpu_thread;
static bool s_gpu_wanted;
/* Asked of the GPU thread by the frontend's, which wakes it with to_gpu
 * while it is parked; what it reports, a field ended or its parking,
 * wakes the frontend's thread with from_gpu. */
static bool s_gpu_events_ready;
static retro_eventcount_t s_gpu_to_gpu;
static retro_eventcount_t s_gpu_from_gpu;
static retro_atomic_int_t s_gpu_quit;
static retro_atomic_int_t s_gpu_park;
static retro_atomic_int_t s_gpu_parked;
static retro_atomic_int_t s_gpu_fields;
static int s_gpu_fields_seen;  // the frontend's thread only

#ifdef _WIN32
/* Direct3D 11 version 2: the interface, and the levels of its context
 * lock this thread holds */
static retro_hw_render_interface_d3d11* s_d3d11_v2;
static unsigned s_d3d11_depth;
#endif

int GetAdjustedBaseWidth()
{
  // VR packs both eyes side by side, each at the full width.
  return VR::IsEnabled() ? EFB_WIDTH * 2 : EFB_WIDTH;
}

int GetAdjustedBaseHeight()
{
  const bool crop_overscan = Libretro::Options::GetCached<bool>(
    Libretro::Options::gfx_settings::CROP_OVERSCAN);

  if (crop_overscan)
  {
    // A 480-line signal (native NTSC, or a PAL game that switched to PAL60/EuRGB60 at runtime)
    // should report 480 so the frontend / CRT switchres picks a 480i@60 mode instead of stretching
    // a 576-line PAL mode down to ~52 Hz. Detect the 60 Hz output from the current refresh rate,
    // since the cartridge region stays PAL even after the in-game 60 Hz switch.
    const bool is_480_line_output =
      retro_get_region() == RETRO_REGION_NTSC ||
      Core::System::GetInstance().GetVideoInterface().GetTargetRefreshRate() > 55.0;

    return is_480_line_output ? 480 : 576;
  }

  return EFB_HEIGHT;
}

void Init()
{
  DEBUG_LOG(VIDEO, "Video - Init");

  const char* renderer = Libretro::Options::GetCached<const char*>(
    Libretro::Options::gfx_settings::RENDERER, "");

  if (!strcmp(renderer, "Hardware"))
  {
    retro_hw_context_type preferred = RETRO_HW_CONTEXT_NONE;
    if (environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred))
    {
      switch (preferred)
      {
#ifdef HAS_OPENGL
        case RETRO_HW_CONTEXT_OPENGLES_VERSION:
          if (SetHWRender(preferred, 3, 2))
            return;
          if (SetHWRender(preferred, 3, 1))
            return;
          break;
#endif
        default:
          if (SetHWRender(preferred))
            return;
      }
    }
#if defined(HAS_OPENGL) && !defined(ANDROID) && !defined(__WEBOS__)
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGL_CORE))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGL))
      return;
#endif
#ifdef HAS_OPENGL
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES_VERSION, 3, 2))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES_VERSION, 3, 1))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_OPENGLES3))
      return;
#endif
#ifdef _WIN32
    if (SetHWRender(RETRO_HW_CONTEXT_D3D12))
      return;
    if (SetHWRender(RETRO_HW_CONTEXT_D3D11))
      return;
#endif
#ifdef HAS_VULKAN
    if (SetHWRender(RETRO_HW_CONTEXT_VULKAN))
      return;
#endif
  }
  hw_render.context_type = RETRO_HW_CONTEXT_NONE;
  if (!strcmp(renderer, "Software"))
    Config::SetBase(Config::MAIN_GFX_BACKEND, "Software Renderer");
  else
    Config::SetBase(Config::MAIN_GFX_BACKEND, "Null");
}

void D3D11Lock(void)
{
#ifdef _WIN32
  bool used;
  if (!s_d3d11_v2)
    return;
  used = s_d3d11_v2->lock_context(s_d3d11_v2->handle);
  /* the frontend had the context: everything bound goes back */
  if (s_d3d11_depth++ == 0 && used && g_gfx && DX11::D3D::stateman)
    static_cast<DX11::Gfx*>(g_gfx)->RestoreContext();
#endif
}

void D3D11Unlock(void)
{
#ifdef _WIN32
  if (!s_d3d11_v2 || !s_d3d11_depth)
    return;
  s_d3d11_depth--;
  s_d3d11_v2->unlock_context(s_d3d11_v2->handle);
#endif
}

unsigned D3D11Suspend(void)
{
  unsigned depth = 0;
#ifdef _WIN32
  depth = s_d3d11_depth;
  while (s_d3d11_depth)
    D3D11Unlock();
#endif
  return depth;
}

void D3D11Resume(unsigned depth)
{
  while (depth--)
    D3D11Lock();
}

bool SetHWRender(retro_hw_context_type type, const int version_major, const int version_minor)
{
  DEBUG_LOG(VIDEO, "Video - SetHWRender!");

  hw_render.context_type = type;
  hw_render.context_reset = ContextReset;
  hw_render.context_destroy = ContextDestroy;
  hw_render.bottom_left_origin = true;
  bool success = false;

  switch (type)
  {
#ifdef HAS_OPENGL
  case RETRO_HW_CONTEXT_OPENGL_CORE:
    // minimum requirements to run is opengl 3.3 (RA will try to use highest version available anyway)
    hw_render.version_major = (version_major != -1) ? version_major : 3;
    hw_render.version_minor = (version_minor != -1) ? version_minor : 3;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      Config::SetBase(Config::MAIN_GFX_BACKEND, "OGL");
      success = true;
    } else {
      WARN_LOG(VIDEO, "Video - SetHWRender - failed to set hw renderer for OpenGL Core");
    }
    break;
  case RETRO_HW_CONTEXT_OPENGLES_VERSION:
  case RETRO_HW_CONTEXT_OPENGLES3:
  case RETRO_HW_CONTEXT_OPENGL:
  {
    // when using RETRO_HW_CONTEXT_OPENGL you can't set version above 3.0 (RA will try to use highest version available anyway)
    // dolphin support OpenGL ES 3.0 too (no support for 2.0) so we are good
    hw_render.version_major = (version_major != -1) ? version_major : 3;
    hw_render.version_minor = (version_minor != -1) ? version_minor : 0;

    char api_name[32];
    snprintf(api_name, sizeof(api_name), "%s %u.%u",
             (type == RETRO_HW_CONTEXT_OPENGLES3 || type == RETRO_HW_CONTEXT_OPENGLES_VERSION) ?
                 "OpenGL ES" :
                 "OpenGL",
             hw_render.version_major, hw_render.version_minor);

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      // Shared context is required with "gl" video driver
      if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr)) {
        WARN_LOG(VIDEO, "Video - SetHWRender - unable to set shared context for %s",
                 api_name);
      }
      INFO_LOG(VIDEO, "Video - SetHWRender - using %s", api_name);
      Config::SetBase(Config::MAIN_GFX_BACKEND, "OGL");
      success = true;
    }
    else
    {
      WARN_LOG(VIDEO, "Video - SetHWRender - failed to set hw renderer for %s", api_name);
    }
    break;
  }
#endif
#ifdef _WIN32
  case RETRO_HW_CONTEXT_D3D11:
    hw_render.version_major = 11;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      /* version 2 of the interface, when the frontend has it: the frame
       * is read where it is drawn, from whichever thread presents */
      static const struct retro_hw_render_context_negotiation_interface_d3d11 negotiation = {
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11,
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11_VERSION,
          RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2};
      struct retro_hw_render_context_negotiation_interface probe;
      probe.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11;
      probe.interface_version = 0;
      if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT,
                     &probe) &&
          probe.interface_version >= 1)
      {
        environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
                   (void*)&negotiation);
      }
      Config::SetBase(Config::MAIN_GFX_BACKEND, "D3D");
      success = true;
    }
    break;
  case RETRO_HW_CONTEXT_D3D12:
    hw_render.version_major = 12;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      /* version 2 of the interface, when the frontend has it: the frame
       * is read where it is drawn, not copied */
      static const struct retro_hw_render_context_negotiation_interface_d3d12 negotiation = {
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12,
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12_VERSION,
          RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2};
      struct retro_hw_render_context_negotiation_interface probe;
      probe.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12;
      probe.interface_version = 0;
      if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT,
                     &probe) &&
          probe.interface_version >= 1)
      {
        environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
                   (void*)&negotiation);
      }
      Config::SetBase(Config::MAIN_GFX_BACKEND, "D3D12");
      success = true;
    }
    break;
#endif
#ifdef HAS_VULKAN
  case RETRO_HW_CONTEXT_VULKAN:
    hw_render.version_major = VK_API_VERSION_1_0;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
      static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
          Vk::GetApplicationInfo,
          Vk::CreateDevice,
          NULL,  // destroy_device
#ifdef __APPLE__
          Vk::CreateInstance,  // create_instance (v2 API)
#else
          NULL,  // create_instance
#endif
          Vk::CreateDevice2,  // create_device2 (v2 API)
      };
      environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);

      Config::SetBase(Config::MAIN_GFX_BACKEND, "Vulkan");
      success = true;
    }
    break;
#endif
  default:
    break;
  }

  return success;
}

#ifdef _WIN32
/* The Direct3D 11 backend on the frontend's device, with the context
 * lock held. */
static void ContextResetD3D11(retro_hw_render_interface_d3d11* d3d, int efbScale)
{
  DX11::D3D::device = d3d->device;
  DX11::D3D::context = d3d->context;
  DX11::D3D::feature_level = d3d->featureLevel;

  if (FAILED(DX11::D3D::device.As(&DX11::D3D::device1)))
  {
    WARN_LOG(VIDEO, "Missing Direct3D 11.1 support. Logical operations will not be supported.");
    g_backend_info.bSupportsLogicOp = false;
  }

  DX11::D3D::stateman = new DX11::D3D::StateManager();

  DX11::VideoBackend* d3d11 = static_cast<DX11::VideoBackend*>(g_video_backend);

  if (!d3d11)
  {
    WARN_LOG(VIDEO, "D3D11 backend is null");
    return;
  }

  d3d11->FillD3DBackendInfo();
  g_Config.VerifyValidity();
  UpdateActiveConfig();

  DX11Output* const output = new DX11Output(
    s_d3d11_v2, GetAdjustedBaseWidth() * efbScale, GetAdjustedBaseHeight() * efbScale);
  if (!output->Create())
  {
    delete output;
    return;
  }

  /* made in this order, each taken by InitializeShared */
  AbstractGfx* const gfx = new DX11::Gfx(output, static_cast<float>(efbScale));
  VertexManagerBase* const vertex_manager = new DX11::VertexManager();
  PerfQueryBase* const perf_query = new DX11::PerfQuery();
  BoundingBox* const bounding_box = new DX11::D3DBoundingBox();

  if (d3d11->InitializeShared(gfx, vertex_manager, perf_query, bounding_box))
    g_context_status.MarkInitialized();
}
#endif

void ContextReset(void)
{
  DEBUG_LOG(VIDEO, "Video - ContextReset!");

  g_context_status.MarkReset();

#ifdef HAS_VULKAN
  if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
  {
    retro_hw_render_interface* vulkan;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&vulkan) ||
        !vulkan)
    {
      ERROR_LOG(VIDEO, "Failed to get HW rendering interface!");
      return;
    }
    if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
    {
      ERROR_LOG(VIDEO, "HW render interface mismatch, expected %d, got %u",
                RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
      return;
    }
    Vk::SetHWRenderInterface(vulkan);
  }
#endif

#ifdef _WIN32
  if (hw_render.context_type == RETRO_HW_CONTEXT_D3D11)
  {
    int efbScale = Libretro::Options::GetCached<int>(
      Libretro::Options::gfx_settings::EFB_SCALE, 1);

    retro_hw_render_interface_d3d11* d3d;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&d3d) || !d3d)
    {
      ERROR_LOG(VIDEO, "Failed to get HW rendering interface!");
      return;
    }
    if (d3d->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION &&
        d3d->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2)
    {
      ERROR_LOG(VIDEO, "HW render interface mismatch, expected %d or %d, got %u!",
                RETRO_HW_RENDER_INTERFACE_D3D11_VERSION, RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2,
                d3d->interface_version);
      return;
    }
    s_d3d11_v2 = d3d->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2 ? d3d : nullptr;
    s_d3d11_depth = 0;
    D3D11Lock();
    ContextResetD3D11(d3d, efbScale);
    D3D11Unlock();
    return;
  }

  if (hw_render.context_type == RETRO_HW_CONTEXT_D3D12)
  {
    int efbScale = Libretro::Options::GetCached<int>(
      Libretro::Options::gfx_settings::EFB_SCALE, 1);

    retro_hw_render_interface_d3d12* d3d12;
    if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&d3d12) || !d3d12)
    {
      ERROR_LOG(VIDEO, "Failed to get HW rendering interface!");
      return;
    }

    if (d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION &&
        d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2)
    {
      ERROR_LOG(VIDEO, "HW render interface mismatch, expected %d or %d, got %u!",
                RETRO_HW_RENDER_INTERFACE_D3D12_VERSION, RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2,
                d3d12->interface_version);
      return;
    }

    // Create D3D12 context with RetroArch's device and queue
    ID3D12Device* ra_device = static_cast<ID3D12Device*>(d3d12->device);
    ID3D12CommandQueue* ra_queue = static_cast<ID3D12CommandQueue*>(d3d12->queue);

    if (!DX12::DXContext::CreateWithExternalDevice(ra_device, ra_queue))
    {
      ERROR_LOG(VIDEO, "Failed to create D3D12 context with external device");
      return;
    }

    static_cast<DX12::VideoBackend*>(g_video_backend)->FillD3D12BackendInfo();
    g_Config.VerifyValidity();

    Microsoft::WRL::ComPtr<ID3D12Device1> device1;
    if (FAILED(DX12::g_dx_context->GetDevice()->QueryInterface(IID_PPV_ARGS(&device1))))
    {
      WARN_LOG(VIDEO, "Missing Direct3D 12.1+ support. Logical operations will not be supported.");
      g_backend_info.bSupportsLogicOp = false;
    }

    if (!DX12::g_dx_context->CreateGlobalResources())
    {
      ERROR_LOG(VIDEO, "Failed to create D3D12 global resources");
      DX12::DXContext::Destroy();
      return;
    }

    UpdateActiveConfig();

    s_gpu_wanted = Config::Get(Config::MAIN_CPU_THREAD);
    DX12Output* const output = new DX12Output(
        d3d12, GetAdjustedBaseWidth() * efbScale, GetAdjustedBaseHeight() * efbScale,
        s_gpu_wanted);
    if (!output->Create())
    {
      delete output;
      DX12::DXContext::Destroy();
      return;
    }

    AbstractGfx* const gfx = new DX12::Gfx(output, static_cast<float>(efbScale));
    VertexManagerBase* const vertex_manager = new DX12::VertexManager();
    PerfQueryBase* const perf_query = new DX12::PerfQuery();
    BoundingBox* const bounding_box = new DX12::D3D12BoundingBox();

    if (!g_video_backend->InitializeShared(gfx, vertex_manager, perf_query, bounding_box))
    {
      ERROR_LOG(VIDEO, "Failed to initialize shared components");
      DX12::DXContext::Destroy();
      return;
    }

    g_context_status.MarkInitialized();

    return;
  }
#endif
  if(Video_InitializeBackend())
    g_context_status.MarkInitialized();
}

bool Video_InitializeBackend()
{
  const float efbScale = static_cast<float>(Libretro::Options::GetCached<int>(
    Libretro::Options::gfx_settings::EFB_SCALE, 1));

#ifdef HAS_VULKAN
  if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
  {
    // Since VulkanContext maintains a copy of the device features and properties, we can use this
    // to initialize the backend information, so that we don't need to enumerate everything again.
    Vulkan::VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, Vulkan::g_vulkan_context->GetPhysicalDevice(),
                                              Vulkan::g_vulkan_context->GetDeviceInfo());
    Vulkan::VulkanContext::PopulateBackendInfoMultisampleModes(
        &g_backend_info, Vulkan::g_vulkan_context->GetPhysicalDevice(), Vulkan::g_vulkan_context->GetDeviceInfo());
    Vulkan::VKPipeline::PopulateBackendInfoDrawState(&g_backend_info);
    UpdateActiveConfig();

    // Remaining classes are also dependent on object cache.
    Vulkan::g_object_cache = new Vulkan::ObjectCache();
    if (!Vulkan::g_object_cache->Initialize())
    {
      PanicAlert("Failed to initialize Vulkan object cache.");
      return false;
    }

    // Create command buffers. We do this separately because the other classes depend on it.
    Vulkan::g_command_buffer_mgr = new Vulkan::CommandBufferManager(g_Config.bBackendMultithreading);
    if (!Vulkan::g_command_buffer_mgr->Initialize())
    {
      PanicAlert("Failed to create Vulkan command buffers");
      return false;
    }

    if (!Vulkan::StateTracker::CreateInstance())
    {
      PanicAlert("Failed to create state tracker");
      return false;
    }

    /* where the picture goes: the frontend's sync images, sized as the
     * frame the frontend was told of */
    s_gpu_wanted = Config::Get(Config::MAIN_CPU_THREAD);
    VKOutput* const output = new VKOutput(
        Vk::GetHWRenderInterface(), GetAdjustedBaseWidth() * efbScale,
        GetAdjustedBaseHeight() * efbScale, s_gpu_wanted);
    if (!output->Create())
    {
      delete output;
      return false;
    }

    AbstractGfx* const gfx = new Vulkan::VKGfx(output, efbScale);
    VertexManagerBase* const vertex_manager = new Vulkan::VertexManager();
    PerfQueryBase* const perf_query = new Vulkan::PerfQuery();
    BoundingBox* const bounding_box = new Vulkan::VKBoundingBox();

    return g_video_backend->InitializeShared(gfx, vertex_manager, perf_query, bounding_box);
  }
#endif

  if(!g_video_backend) {
    WARN_LOG(VIDEO, "Video - g_video_backend - No supported renderer found");
    return false;
  }

  // this calls InitializeGLExtensions, FillBackendInfo and InitializeShared internally
  return g_video_backend->Initialize(efbScale);
}

void ContextDestroy(void)
{
  DEBUG_LOG(VIDEO, "Video - ContextDestroy!");

  /* the backend goes away with the context: its thread first */
  GpuThreadStop();
  s_gpu_wanted = false;

  g_context_status.MarkDestroyed();

  /* the backend's teardown uses the context */
  D3D11Lock();

#ifdef HAS_OPENGL
  if (g_gfx &&
      Config::Get(Config::MAIN_GFX_BACKEND) == "OGL")
  {
    static_cast<OGL::OGLGfx*>(g_gfx)->SetSystemFrameBuffer(0);
  }
#endif

  if (g_video_backend)
  {
    g_video_backend->Shutdown();
  }

  switch (hw_render.context_type)
  {
  case RETRO_HW_CONTEXT_D3D12:
#ifdef _WIN32
    DX12::DXContext::Destroy();
#endif
    break;
  case RETRO_HW_CONTEXT_D3D11:
#ifdef _WIN32
    if (DX11::D3D::context)
    {
      DX11::D3D::context->ClearState();
      DX11::D3D::context->Flush();
    }

    DX11::D3D::context.Reset();
    DX11::D3D::device1.Reset();

    DX11::D3D::device = nullptr;
    DX11::D3D::device1 = nullptr;
    DX11::D3D::context = nullptr;
    delete DX11::D3D::stateman;
    DX11::D3D::stateman = nullptr;
#endif
    break;
  case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAS_VULKAN
    Vk::SetHWRenderInterface(nullptr);
#endif
    break;
  case RETRO_HW_CONTEXT_OPENGL:
  case RETRO_HW_CONTEXT_OPENGL_CORE:
  case RETRO_HW_CONTEXT_OPENGLES3:
    break;
  default:
  case RETRO_HW_CONTEXT_NONE:
    break;
  }
  D3D11Unlock();
#ifdef _WIN32
  s_d3d11_v2 = nullptr;
  s_d3d11_depth = 0;
#endif
}
#ifdef HAS_VULKAN
namespace Vk
{
/* The instance the frontend makes for the core: the version the loader
 * has, up to 1.2, as the backend calls the 1.1 functions (the Properties2
 * and Features2 queries and the feature chain they fill) where the device
 * has them, which a 1.0 instance leaves the loader emulating without the
 * chain. */
static u32 s_instance_api_version = VK_API_VERSION_1_0;

const VkApplicationInfo* GetApplicationInfo(void)
{
  static VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "Dolphin-Emu";
  app_info.applicationVersion = 5;  // TODO: extract from Common::scm_desc_str
  app_info.pEngineName = "Dolphin-Emu";
  app_info.engineVersion = 2;
  s_instance_api_version = Vulkan::LoadVulkanLibrary() ?
                               Vulkan::VulkanContext::GetInstanceAPIVersion() :
                               VK_API_VERSION_1_0;
  app_info.apiVersion = s_instance_api_version;
  return &app_info;
}

#ifdef __APPLE__
VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                          const VkApplicationInfo* app,
                          retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
                          void* opaque)
{
  VkInstanceCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = app;

  static const char* const extensions[] = {VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};

  create_info.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
  create_info.ppEnabledExtensionNames = extensions;


  static VkBool32 disable_arg_buffers = VK_FALSE;

  VkLayerSettingEXT layer_setting = {};
  layer_setting.pLayerName = "MoltenVK";
  layer_setting.pSettingName = "MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS";
  layer_setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
  layer_setting.valueCount = 1;
  layer_setting.pValues = &disable_arg_buffers;

  VkLayerSettingsCreateInfoEXT layer_settings = {};
  layer_settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
  layer_settings.settingCount = 1;
  layer_settings.pSettings = &layer_setting;

  create_info.pNext = &layer_settings;

  return create_instance_wrapper(opaque, &create_info);
}
#endif

static bool CreateDeviceInternal(
    retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
    unsigned num_required_device_extensions, const char** required_device_layers,
    unsigned num_required_device_layers, const VkPhysicalDeviceFeatures* required_features,
    retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
  assert(g_video_backend->GetConfigName() == "Vulkan");

  Vulkan::LoadVulkanLibrary();

  Init(instance, gpu, get_instance_proc_addr, required_device_extensions,
       num_required_device_extensions, required_device_layers, num_required_device_layers,
       required_features, create_device_wrapper, opaque);

  if (!Vulkan::LoadVulkanInstanceFunctions(instance))
  {
    ERROR_LOG(VIDEO, "Failed to load Vulkan instance functions.");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  Vulkan::VulkanContext::GPUList gpu_list = Vulkan::VulkanContext::EnumerateGPUs(instance);
  if (gpu_list.empty())
  {
    ERROR_LOG(VIDEO, "No Vulkan physical devices available.");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  Vulkan::VulkanContext::PopulateBackendInfo(&g_backend_info);
  Vulkan::VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);

  if (gpu == VK_NULL_HANDLE)
    gpu = gpu_list[0];
  Vulkan::g_vulkan_context =
      Vulkan::VulkanContext::Create(instance, gpu, surface, false, false, s_instance_api_version);
  if (!Vulkan::g_vulkan_context)
  {
    ERROR_LOG(VIDEO, "Failed to create Vulkan device");
    Vulkan::UnloadVulkanLibrary();
    return false;
  }

  context->gpu = Vulkan::g_vulkan_context->GetPhysicalDevice();
  context->device = Vulkan::g_vulkan_context->GetDevice();
  context->queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
  context->queue_family_index = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
  /* the frontend presents; the queue the surface takes may be another
   * family's */
  context->presentation_queue = context->queue;
  context->presentation_queue_family_index = context->queue_family_index;
  if (Vulkan::g_vulkan_context->GetPresentQueue() != VK_NULL_HANDLE)
  {
    context->presentation_queue = Vulkan::g_vulkan_context->GetPresentQueue();
    context->presentation_queue_family_index =
        Vulkan::g_vulkan_context->GetPresentQueueFamilyIndex();
  }

  return true;
}

bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                  VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                  const char** required_device_extensions, unsigned num_required_device_extensions,
                  const char** required_device_layers, unsigned num_required_device_layers,
                  const VkPhysicalDeviceFeatures* required_features)
{
  return CreateDeviceInternal(context, instance, gpu, surface, get_instance_proc_addr,
                              required_device_extensions, num_required_device_extensions,
                              required_device_layers, num_required_device_layers, required_features,
                              nullptr, nullptr);
}

bool CreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                   VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                   retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
  if (!create_device_wrapper)
  {
    ERROR_LOG(VIDEO, "Vulkan negotiation v2 requires a device creation wrapper.");
    return false;
  }

  return CreateDeviceInternal(context, instance, gpu, surface, get_instance_proc_addr, nullptr, 0,
                              nullptr, 0, nullptr, create_device_wrapper, opaque);
}
}  // namespace Vk
#endif


static void GpuThreadMain(void* data)
{
  Core::System& system = Core::System::GetInstance();
  (void)data;

  Core::DeclareAsGPUThread();
  for (;;)
  {
    if (retro_atomic_load_acquire_int(&s_gpu_quit))
      break;
    if (retro_atomic_load_acquire_int(&s_gpu_park))
    {
      retro_atomic_store_release_int(&s_gpu_parked, 1);
      retro_eventcount_notify(&s_gpu_from_gpu);
      for (;;)
      {
        const int key = retro_eventcount_prepare_wait(&s_gpu_to_gpu);
        if (!retro_atomic_load_acquire_int(&s_gpu_park) ||
            retro_atomic_load_acquire_int(&s_gpu_quit))
        {
          retro_eventcount_cancel_wait(&s_gpu_to_gpu);
          break;
        }
        retro_eventcount_commit_wait(&s_gpu_to_gpu, key);
      }
      retro_atomic_store_release_int(&s_gpu_parked, 0);
      /* against GpuThreadPark: its request is seen here, or this unparking is seen there */
      retro_atomic_thread_fence_seq_cst();
      continue;
    }
    if (system.GetFifo().RunGpuLoop(0) == Fifo::FifoManager::GPU_LOOP_FIELD)
    {
      retro_atomic_fetch_add_int(&s_gpu_fields, 1);
      retro_eventcount_notify(&s_gpu_from_gpu);
    }
  }
  retro_atomic_store_release_int(&s_gpu_parked, 1);
  retro_eventcount_notify(&s_gpu_from_gpu);
}

bool GpuThreadWanted()
{
  return s_gpu_wanted;
}

bool GpuThreadRunning()
{
  return s_gpu_thread != nullptr;
}

void GpuThreadEnsure()
{
  if (s_gpu_thread || !s_gpu_wanted || !Core::System::GetInstance().IsDualCoreMode())
    return;
  if (!s_gpu_events_ready)
  {
    if (!retro_eventcount_init(&s_gpu_to_gpu))
      return;
    if (!retro_eventcount_init(&s_gpu_from_gpu))
    {
      retro_eventcount_free(&s_gpu_to_gpu);
      return;
    }
    s_gpu_events_ready = true;
  }
  retro_atomic_store_release_int(&s_gpu_quit, 0);
  retro_atomic_store_release_int(&s_gpu_park, 0);
  retro_atomic_store_release_int(&s_gpu_parked, 0);
  s_gpu_fields_seen = retro_atomic_load_acquire_int(&s_gpu_fields);
  VideoCommon::g_frontend_takes_frames = true;
  s_gpu_thread = sthread_create(GpuThreadMain, nullptr);
}

void GpuThreadStop()
{
  if (!s_gpu_thread)
    return;
  retro_atomic_store_release_int(&s_gpu_quit, 1);
  retro_eventcount_notify(&s_gpu_to_gpu);
  Core::System::GetInstance().GetFifo().InterruptGpuLoop();
  sthread_join(s_gpu_thread);
  s_gpu_thread = nullptr;
  VideoCommon::g_frontend_takes_frames = false;
}

bool GpuThreadWaitField(int64_t wedge_us)
{
  int fields;

  if (!s_gpu_thread)
    return false;
  for (;;)
  {
    int key;
    if (retro_atomic_load_acquire_int(&s_gpu_fields) != s_gpu_fields_seen ||
        retro_atomic_load_acquire_int(&s_gpu_quit))
      break;
    key = retro_eventcount_prepare_wait(&s_gpu_from_gpu);
    if (retro_atomic_load_acquire_int(&s_gpu_fields) != s_gpu_fields_seen ||
        retro_atomic_load_acquire_int(&s_gpu_quit))
    {
      retro_eventcount_cancel_wait(&s_gpu_from_gpu);
      break;
    }
    if (!wedge_us)
      retro_eventcount_commit_wait(&s_gpu_from_gpu, key);
    else if (!retro_eventcount_commit_wait_timeout(&s_gpu_from_gpu, key, wedge_us))
      break; /* wedged: the frontend gets a repeat, the field a later run */
  }
  fields = retro_atomic_load_acquire_int(&s_gpu_fields);
  const bool ended = fields != s_gpu_fields_seen;
  s_gpu_fields_seen = fields;
  return ended;
}

void GpuThreadPark()
{
  if (!s_gpu_thread)
    return;
  retro_atomic_store_release_int(&s_gpu_park, 1);
  /* against the GPU thread's unparking: it sees this request, or this sees it running */
  retro_atomic_thread_fence_seq_cst();
  Core::System::GetInstance().GetFifo().InterruptGpuLoop();
  for (;;)
  {
    const int key = retro_eventcount_prepare_wait(&s_gpu_from_gpu);
    if (retro_atomic_load_acquire_int(&s_gpu_parked))
    {
      retro_eventcount_cancel_wait(&s_gpu_from_gpu);
      break;
    }
    retro_eventcount_commit_wait(&s_gpu_from_gpu, key);
  }
  /* a field swapped while the frontend's thread has the video state,
   * a savestate's say, is not presented */
  VideoCommon::g_frontend_takes_frames = false;
}

void GpuThreadUnpark()
{
  if (!s_gpu_thread)
    return;
  VideoCommon::g_frontend_takes_frames = true;
  retro_atomic_store_release_int(&s_gpu_park, 0);
  retro_eventcount_notify(&s_gpu_to_gpu);
}

/* The frontend's callback, behind one that counts what each retro_run
 * hands over: a run whose fields only repeated the last picture, or that
 * ran out of time before its field ended, draws nothing and reports a
 * repeat, with the last picture's size.
 *
 * A picture drawn or handed over on the hardware is reported at the end
 * of the run, the last one drawn: the GPU loop can catch up on two
 * fields in one run, after one that ran out of time, and the frontend
 * takes one frame a run. Pictures in memory, and those the frontend
 * reads inside the callback, go to it at once, the first of a run. */
static retro_video_refresh_t s_frontend_video_cb;
static unsigned s_frames_sent;
static bool s_frame_drawn;
static unsigned s_last_width;
static unsigned s_last_height;

static void CountedVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch)
{
  if (data == RETRO_HW_FRAME_BUFFER_VALID)
  {
    s_frame_drawn = true;
    s_last_width = width;
    s_last_height = height;
    return;
  }
  if (s_frames_sent)
    return;
  s_frames_sent++;
  if (data)
  {
    s_last_width = width;
    s_last_height = height;
  }
  s_frontend_video_cb(data, width, height, pitch);
}

void RefreshNow(unsigned width, unsigned height)
{
  if (s_frames_sent)
    return;
  s_frames_sent++;
  s_frame_drawn = false;
  s_last_width = width;
  s_last_height = height;
  s_frontend_video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
}

void EndFrame()
{
  OutputHandOver();
  if (s_frame_drawn && !s_frames_sent)
  {
    OutputKeepReadable();
    s_frames_sent++;
    s_frontend_video_cb(RETRO_HW_FRAME_BUFFER_VALID, s_last_width, s_last_height, 0);
  }
  s_frame_drawn = false;
  if (!s_frames_sent)
  {
    /* before the first picture, the size the frontend was told */
    if (!s_last_width || !s_last_height)
    {
      retro_system_av_info info;
      retro_get_system_av_info(&info);
      s_last_width = info.geometry.base_width;
      s_last_height = info.geometry.base_height;
    }
    OutputKeepReadable();
    s_frontend_video_cb(nullptr, s_last_width, s_last_height, 0);
  }
  s_frames_sent = 0;
}
}  // namespace Video
}  // namespace Libretro

void retro_set_video_refresh(retro_video_refresh_t cb)
{
  Libretro::Video::s_frontend_video_cb = cb;
  Libretro::Video::video_cb = Libretro::Video::CountedVideoRefresh;
}
