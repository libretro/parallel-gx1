// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoBackendBase.h"

#include "VideoCommon/ShaderArena.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <cstdio>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

// TODO: ugly
#ifdef _WIN32
#include "VideoBackends/D3D/VideoBackend.h"
#include "VideoBackends/D3D12/VideoBackend.h"
#endif
#include "VideoBackends/Null/VideoBackend.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/VideoBackend.h"
#include "VideoBackends/Software/VideoBackend.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VideoBackend.h"
#endif

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GBAScreens.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoState.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFStateManager.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"

VideoBackendBase* g_video_backend = nullptr;

#ifdef _WIN32
#include <windows.h>

// Nvidia drivers >= v302 will check if the application exports a global
// variable named NvOptimusEnablement to know if it should run the app in high
// performance graphics mode or using the IGP.
// AMD drivers >= 13.35 do the same, but for the variable
// named AmdPowerXpressRequestHighPerformance instead.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

std::string VideoBackendBase::BadShaderFilename(const char* shader_stage, int counter)
{
  return StringFromFormat("%sbad_%s_%s_%d.txt", File::GetUserPath(D_DUMP_IDX).c_str(), shader_stage,
                          g_video_backend->GetConfigName().c_str(), counter);
}

// Run from the CPU thread (from VideoInterface.cpp)
struct ViSwapEvent
{
  u32 xfb_addr;
  u32 fb_width;
  u32 fb_stride;
  u32 fb_height;
  u64 ticks;
};

struct BoundingBoxRead
{
  u32 index;
  u16 value;
};

static void ViSwapOnGPU(void* data)
{
  const ViSwapEvent* const swap = static_cast<const ViSwapEvent*>(data);
  g_presenter->ViSwap(swap->xfb_addr, swap->fb_width, swap->fb_stride, swap->fb_height,
                      swap->ticks);
}

static void SetNextSwapOnGPU(void* data)
{
  u64 ticks;
  std::memcpy(&ticks, data, sizeof(ticks));
  g_presenter->SetNextSwapEstimatedTicks(ticks);
}

static void FlushPerfQueryOnGPU(void*)
{
  g_perf_query->FlushResults();
}

static void ReadBoundingBoxOnGPU(void* data)
{
  BoundingBoxRead* const read = static_cast<BoundingBoxRead*>(data);
  read->value = g_bounding_box->Get(read->index);
}

static void DoStateOnGPU(void* data)
{
  VideoCommon_DoState(*static_cast<PointerWrap*>(data));
}

void VideoBackendBase::Video_OutputXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height,
                                       u64 ticks)
{
  if (!m_initialized || !g_presenter)
    return;

  auto& system = Core::System::GetInstance();

  if (!g_ActiveConfig.bImmediateXFB)
  {
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Swap);

    ViSwapEvent swap;
    swap.xfb_addr = xfb_addr;
    swap.fb_width = fb_width;
    swap.fb_stride = fb_stride;
    swap.fb_height = fb_height;
    swap.ticks = ticks;
    AsyncRequests::GetInstance()->PushEvent(ViSwapOnGPU, &swap, sizeof(swap));
  }

  // Inform the Presenter of the next estimated swap time.

  auto& vi = system.GetVideoInterface();
  const s64 refresh_rate_den = vi.GetTargetRefreshRateDenominator();
  const s64 refresh_rate_num = vi.GetTargetRefreshRateNumerator();

  const u64 next_swap_estimated_ticks =
      ticks + (system.GetSystemTimers().GetTicksPerSecond() * refresh_rate_den / refresh_rate_num);
  AsyncRequests::GetInstance()->PushEvent(SetNextSwapOnGPU, &next_swap_estimated_ticks,
                                          sizeof(next_swap_estimated_ticks));
}

u32 VideoBackendBase::Video_GetQueryResult(PerfQueryType type)
{
  if (!g_perf_query->ShouldEmulate())
  {
    return 0;
  }

  auto& system = Core::System::GetInstance();
  system.GetFifo().SyncGPU(Fifo::SyncGPUReason::PerfQuery);

  if (!g_perf_query->IsFlushed())
  {
    AsyncRequests::GetInstance()->PushBlockingEvent(FlushPerfQueryOnGPU, nullptr);
  }

  return g_perf_query->GetQueryResult(type);
}

u16 VideoBackendBase::Video_GetBoundingBox(int index)
{

  if (!g_ActiveConfig.bBBoxEnable)
  {
    static bool warn_once = true;
    if (warn_once)
    {
      ERROR_LOG(VIDEO, "BBox shall be used but it is disabled. Please use a gameini to enable it "
                       "for this game.");
    }
    warn_once = false;
  }
  else if (!g_backend_info.bSupportsBBox)
  {
    static bool warn_once = true;
    if (warn_once)
    {
      PanicAlert("This game requires bounding box emulation to run properly but your graphics "
                 "card or its drivers do not support it. As a result you will experience bugs or "
                 "freezes while running this game.");
    }
    warn_once = false;
  }

  auto& system = Core::System::GetInstance();
  system.GetFifo().SyncGPU(Fifo::SyncGPUReason::BBox);

  BoundingBoxRead read;
  read.index = index;
  AsyncRequests::GetInstance()->PushBlockingEvent(ReadBoundingBoxOnGPU, &read);
  return read.value;
}

// Every backend this build has, the default first
#ifdef _WIN32
static DX11::VideoBackend s_d3d_backend;
static DX12::VideoBackend s_d3d12_backend;
#endif
#ifdef HAS_OPENGL
static OGL::VideoBackend s_ogl_backend;
static SW::VideoSoftware s_software_backend;
#endif
#ifdef HAS_VULKAN
static Vulkan::VideoBackend s_vulkan_backend;
#endif
static Null::VideoBackend s_null_backend;

static VideoBackendBase* const s_backends[] = {
#if defined(HAS_VULKAN) && defined(__APPLE__)
    // On macOS, Vulkan takes precedence over the deprecated OpenGL
    &s_vulkan_backend,
#endif
#ifdef _WIN32
    &s_d3d_backend,      &s_d3d12_backend,
#endif
#ifdef HAS_OPENGL
    &s_ogl_backend,
#endif
#if defined(HAS_VULKAN) && !defined(__APPLE__)
    &s_vulkan_backend,
#endif
#ifdef HAS_OPENGL
    &s_software_backend,
#endif
    &s_null_backend,
};

static VideoBackendBase* GetDefaultVideoBackend()
{
  return s_backends[0];
}

std::string VideoBackendBase::GetDefaultBackendConfigName()
{
  return GetDefaultVideoBackend()->GetConfigName();
}

void VideoBackendBase::ActivateBackend(const std::string& name)
{
  size_t i;
  // If empty or unknown, the default backend stays (expected behavior)
  if (name.empty() || !g_video_backend)
    g_video_backend = GetDefaultVideoBackend();

  for (i = 0; i < sizeof(s_backends) / sizeof(s_backends[0]); i++)
  {
    if (s_backends[i]->GetConfigName() == name)
    {
      g_video_backend = s_backends[i];
      return;
    }
  }
}

#ifdef _WIN32
#define BACKEND_CASES_D3D(q, call)                                                                 \
  case VIDEO_BACKEND_D3D11:                                                                        \
    return static_cast<q DX11::VideoBackend*>(this)->call;                                         \
  case VIDEO_BACKEND_D3D12:                                                                        \
    return static_cast<q DX12::VideoBackend*>(this)->call;
#else
#define BACKEND_CASES_D3D(q, call)
#endif
#ifdef HAS_OPENGL
#define BACKEND_CASES_OGL(q, call)                                                                 \
  case VIDEO_BACKEND_OGL:                                                                          \
    return static_cast<q OGL::VideoBackend*>(this)->call;                                          \
  case VIDEO_BACKEND_SOFTWARE:                                                                     \
    return static_cast<q SW::VideoSoftware*>(this)->call;
#else
#define BACKEND_CASES_OGL(q, call)
#endif
#ifdef HAS_VULKAN
#define BACKEND_CASES_VULKAN(q, call)                                                              \
  case VIDEO_BACKEND_VULKAN:                                                                       \
    return static_cast<q Vulkan::VideoBackend*>(this)->call;
#else
#define BACKEND_CASES_VULKAN(q, call)
#endif

/* The call made on the backend this is; Null for any this build lacks */
#define VIDEO_BACKEND_CALL(q, call)                                                                \
  switch (backend_kind)                                                                            \
  {                                                                                                \
    BACKEND_CASES_D3D(q, call)                                                                     \
    BACKEND_CASES_OGL(q, call)                                                                     \
    BACKEND_CASES_VULKAN(q, call)                                                                  \
  default:                                                                                         \
    break;                                                                                         \
  }                                                                                                \
  return static_cast<q Null::VideoBackend*>(this)->call;

bool VideoBackendBase::Initialize(float backbuffer_scale)
{
  VIDEO_BACKEND_CALL(, Initialize(backbuffer_scale))
}

void VideoBackendBase::Shutdown()
{
  VIDEO_BACKEND_CALL(, Shutdown())
}

void VideoBackendBase::InitBackendInfo(){VIDEO_BACKEND_CALL(, InitBackendInfo())}

std::string VideoBackendBase::GetConfigName() const {VIDEO_BACKEND_CALL(const, GetConfigName())}

std::string VideoBackendBase::GetDisplayName() const
{
  VIDEO_BACKEND_CALL(const, GetDisplayName())
}

void VideoBackendBase::PopulateBackendInfo()
{
  // If the core has been initialized, the backend info will have been populated already. Doing it
  // again would be unnecessary and could cause the UI thread to race with the GPU thread.
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;

  g_Config.Refresh();
  // Reset backend_info so if the backend forgets to initialize something it doesn't end up using
  // a value from the previously used renderer
  g_backend_info = {};
  ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  g_backend_info.DisplayName = g_video_backend->GetDisplayName();
  g_video_backend->InitBackendInfo();
  if (g_backend_info.api_type == APIType::Nothing)
    return;
  // We validate the config after initializing the backend info, as system-specific settings
  // such as anti-aliasing, or the selected adapter may be invalid, and should be checked.
  g_Config.VerifyValidity();
}

void VideoBackendBase::DoState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  if (!system.IsDualCoreMode())
  {
    VideoCommon_DoState(p);
    return;
  }

  AsyncRequests::GetInstance()->PushBlockingEvent(DoStateOnGPU, &p);
}

bool VideoBackendBase::InitializeShared(AbstractGfx* gfx,
                                        VertexManagerBase* vertex_manager,
                                        PerfQueryBase* perf_query,
                                        BoundingBox* bounding_box)
{
  const bool is_initialized = g_context_status.IsInitialized();
  if(!is_initialized)
  {
    memset(reinterpret_cast<u8*>(&g_main_cp_state), 0, sizeof(g_main_cp_state));
    memset(reinterpret_cast<u8*>(&g_preprocess_cp_state), 0, sizeof(g_preprocess_cp_state));
    std::memset(s_tex_mem, 0, sizeof(s_tex_mem));
  }
  // do not initialize again for the config window
  m_initialized = true;

  /* taken: deleted by ShutdownShared */
  g_gfx = gfx;
  g_vertex_manager = vertex_manager;
  g_perf_query = perf_query;
  g_bounding_box = bounding_box;

  g_texture_cache = new TextureCacheBase();

  g_presenter = new VideoCommon::Presenter();
  g_framebuffer_manager = new FramebufferManager();
  g_shader_cache = new VideoCommon::ShaderCache();
  delete g_graphics_mod_manager;
  g_graphics_mod_manager = new GraphicsModManager();
  g_widescreen = new WidescreenManager();

  if (!g_vertex_manager->Initialize() || !g_shader_cache->Initialize() ||
      !g_perf_query->Initialize() || !g_presenter->Initialize() ||
      !g_framebuffer_manager->Initialize(g_ActiveConfig.iEFBScale) ||
      !g_texture_cache->Initialize() ||
      (g_backend_info.bSupportsBBox && !g_bounding_box->Initialize()) ||
      !g_graphics_mod_manager->Initialize())
  {
    PanicAlert("Failed to initialize renderer classes");
    Shutdown();
    return false;
  }

  auto& system = Core::System::GetInstance();
  /* the emulated state, set up once: a context reset mid-game keeps it */
  if (!is_initialized)
  {
    system.GetCommandProcessor().Init();
    system.GetFifo().Init();
    system.GetPixelEngine().Init();
    BPInit();
    VertexLoaderManager::Init();
    system.GetVertexShaderManager().Init();
    system.GetGeometryShaderManager().Init();
    system.GetPixelShaderManager().Init();
    system.GetXFStateManager().Init();
    TMEM::Init();
  }

  g_Config.VerifyValidity();
  UpdateActiveConfig();

  if (g_Config.bDumpTextures)
  {
    Core::DisplayMessage("Texture Dumping is enabled. This will reduce performance.");
  }

  g_shader_cache->InitializeShaderCache();
  system.GetCustomResourceManager().Initialize();

  return true;
}

void VideoBackendBase::ShutdownShared()
{
  auto& system = Core::System::GetInstance();
  system.GetCustomResourceManager().Shutdown();
  /* the GBAs render into the GPU's memory: given back before it goes */
  GBAScreensRevoke();
  delete g_presenter;
  g_presenter = nullptr;

  if (g_shader_cache)
    g_shader_cache->Shutdown();
  if (g_texture_cache)
    g_texture_cache->Shutdown();

  BoundingBoxDelete(g_bounding_box);
  g_bounding_box = nullptr;
  PerfQueryBaseDelete(g_perf_query);
  g_perf_query = nullptr;
  delete g_graphics_mod_manager;
  g_graphics_mod_manager = nullptr;
  delete g_texture_cache;
  g_texture_cache = nullptr;
  TCacheEntryPoolFree();
  delete g_framebuffer_manager;
  g_framebuffer_manager = nullptr;
  delete g_shader_cache;
  g_shader_cache = nullptr;
  /* nothing compiles now: every thread's rslc block goes */
  shader_arenas_free();
  VertexManagerBaseDelete(g_vertex_manager);
  g_vertex_manager = nullptr;
  delete g_widescreen;
  g_widescreen = nullptr;
  AbstractGfxDelete(g_gfx);
  g_gfx = nullptr;

  m_initialized = false;

  /* the loaders stay with the emulated state, for a context given back mid-game */
  VertexLoaderManager::ClearNativeFormats();
}
