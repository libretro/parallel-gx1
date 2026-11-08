// SPDX-License-Identifier: GPL-2.0-or-later

/* The bounding box and the queries call into those of the backend in use
 * (g_video_backend_kind); each backend's are final, so these are direct
 * calls. A backend with none of its own (Null) uses the base classes,
 * which keep nothing on the GPU and count nothing. */

#include <cstring>

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoBackendKind.h"

#include "VideoBackends/Software/SWBoundingBox.h"
#include "VideoBackends/Software/SWPerfQuery.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/OGLBoundingBox.h"
#include "VideoBackends/OGL/OGLPerfQuery.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#endif
#ifdef _WIN32
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DPerfQuery.h"
#include "VideoBackends/D3D12/D3D12BoundingBox.h"
#include "VideoBackends/D3D12/D3D12PerfQuery.h"
#endif

void BoundingBoxDelete(BoundingBox* object)
{
  if (!object)
    return;
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::SWBoundingBox*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLBoundingBox*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKBoundingBox*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::D3DBoundingBox*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::D3D12BoundingBox*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

bool BoundingBox::Initialize()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLBoundingBox*>(this)->Initialize();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKBoundingBox*>(this)->Initialize();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::D3DBoundingBox*>(this)->Initialize();
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::D3D12BoundingBox*>(this)->Initialize();
#endif
  default:
    return true;
  }
}

void BoundingBox::Read(u32 index, u32 length, BBoxType* values)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWBoundingBox*>(this)->Read(index, length, values);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLBoundingBox*>(this)->Read(index, length, values);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKBoundingBox*>(this)->Read(index, length, values);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::D3DBoundingBox*>(this)->Read(index, length, values);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::D3D12BoundingBox*>(this)->Read(index, length, values);
    return;
#endif
  default:
    std::memset(values, 0, sizeof(BBoxType) * length);
    return;
  }
}

void BoundingBox::Write(u32 index, const BBoxType* values, u32 length)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWBoundingBox*>(this)->Write(index, values, length);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLBoundingBox*>(this)->Write(index, values, length);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKBoundingBox*>(this)->Write(index, values, length);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::D3DBoundingBox*>(this)->Write(index, values, length);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::D3D12BoundingBox*>(this)->Write(index, values, length);
    return;
#endif
  default:
    return;
  }
}

void PerfQueryBaseDelete(PerfQueryBase* object)
{
  if (!object)
    return;
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::PerfQuery*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::PerfQuery*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::PerfQuery*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::PerfQuery*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::PerfQuery*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

bool PerfQueryBase::Initialize()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::PerfQuery*>(this)->Initialize();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::PerfQuery*>(this)->Initialize();
#endif
  default:
    return true;
  }
}

void PerfQueryBase::EnableQuery(PerfQueryGroup type)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::PerfQuery*>(this)->EnableQuery(type);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::PerfQuery*>(this)->EnableQuery(type);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::PerfQuery*>(this)->EnableQuery(type);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::PerfQuery*>(this)->EnableQuery(type);
    return;
#endif
  default:
    return;
  }
}

void PerfQueryBase::DisableQuery(PerfQueryGroup type)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::PerfQuery*>(this)->DisableQuery(type);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::PerfQuery*>(this)->DisableQuery(type);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::PerfQuery*>(this)->DisableQuery(type);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::PerfQuery*>(this)->DisableQuery(type);
    return;
#endif
  default:
    return;
  }
}

void PerfQueryBase::ResetQuery()
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::PerfQuery*>(this)->ResetQuery();
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::PerfQuery*>(this)->ResetQuery();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::PerfQuery*>(this)->ResetQuery();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::PerfQuery*>(this)->ResetQuery();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::PerfQuery*>(this)->ResetQuery();
    return;
#endif
  default:
    return;
  }
}

u32 PerfQueryBase::GetQueryResult(PerfQueryType type)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::PerfQuery*>(this)->GetQueryResult(type);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::PerfQuery*>(this)->GetQueryResult(type);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::PerfQuery*>(this)->GetQueryResult(type);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::PerfQuery*>(this)->GetQueryResult(type);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::PerfQuery*>(this)->GetQueryResult(type);
#endif
  default:
    return 0;
  }
}

void PerfQueryBase::FlushResults()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::PerfQuery*>(this)->FlushResults();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::PerfQuery*>(this)->FlushResults();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::PerfQuery*>(this)->FlushResults();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::PerfQuery*>(this)->FlushResults();
    return;
#endif
  default:
    return;
  }
}

bool PerfQueryBase::IsFlushed() const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::PerfQuery*>(this)->IsFlushed();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Vulkan::PerfQuery*>(this)->IsFlushed();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const DX11::PerfQuery*>(this)->IsFlushed();
  case VIDEO_BACKEND_D3D12:
    return static_cast<const DX12::PerfQuery*>(this)->IsFlushed();
#endif
  default:
    return true;
  }
}
