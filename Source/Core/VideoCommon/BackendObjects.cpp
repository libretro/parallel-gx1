// SPDX-License-Identifier: GPL-2.0-or-later

/* The abstract GPU objects call into the classes of the backend in use
 * (g_video_backend_kind): each backend's classes are final, so these are
 * direct calls. */

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/VideoBackendKind.h"

#include "VideoBackends/Null/NullTexture.h"
#include "VideoBackends/Software/SWTexture.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/OGLPipeline.h"
#include "VideoBackends/OGL/OGLShader.h"
#include "VideoBackends/OGL/OGLTexture.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#endif
#ifdef _WIN32
#include "VideoBackends/D3D/DXPipeline.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D12/DX12Pipeline.h"
#include "VideoBackends/D3D12/DX12Shader.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#endif

VideoBackendKind g_video_backend_kind = VIDEO_BACKEND_NULL;

/* shaders */

void std::default_delete<AbstractShader>::operator()(AbstractShader* object) const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLShader*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKShader*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::DXShader*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::DXShader*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

const u8* AbstractShader::GetBinary(size_t* size) const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Vulkan::VKShader*>(this)->GetBinary(size);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const DX11::DXShader*>(this)->GetBinary(size);
  case VIDEO_BACKEND_D3D12:
    return static_cast<const DX12::DXShader*>(this)->GetBinary(size);
#endif
  default:
    *size = 0;
    return nullptr;
  }
}

/* pipelines */

void std::default_delete<AbstractPipeline>::operator()(AbstractPipeline* object) const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLPipeline*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKPipeline*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::DXPipeline*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::DXPipeline*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

AbstractPipeline::CacheData AbstractPipeline::GetCacheData() const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::OGLPipeline*>(this)->GetCacheData();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    return static_cast<const DX12::DXPipeline*>(this)->GetCacheData();
#endif
  default:
    return {};
  }
}

bool AbstractPipeline::IsReady() const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::OGLPipeline*>(this)->IsReady();
#endif
  default:
    return true;
  }
}

bool AbstractPipeline::IsBuilding() const
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::OGLPipeline*>(this)->IsBuilding();
#endif
  default:
    return false;
  }
}

/* framebuffers */

void std::default_delete<AbstractFramebuffer>::operator()(AbstractFramebuffer* object) const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    delete static_cast<Null::NullFramebuffer*>(object);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::SWFramebuffer*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLFramebuffer*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKFramebuffer*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::DXFramebuffer*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::DXFramebuffer*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

/* textures */

void std::default_delete<AbstractTexture>::operator()(AbstractTexture* object) const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    delete static_cast<Null::NullTexture*>(object);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::SWTexture*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLTexture*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKTexture*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::DXTexture*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::DXTexture*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

void AbstractTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                               const MathUtil::Rectangle<int>& src_rect,
                                               u32 src_layer, u32 src_level,
                                               const MathUtil::Rectangle<int>& dst_rect,
                                               u32 dst_layer, u32 dst_level)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullTexture*>(this)->CopyRectangleFromTexture(
        src, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWTexture*>(this)->CopyRectangleFromTexture(src, src_rect, src_layer, src_level,
                                                                dst_rect, dst_layer, dst_level);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLTexture*>(this)->CopyRectangleFromTexture(
        src, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKTexture*>(this)->CopyRectangleFromTexture(
        src, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXTexture*>(this)->CopyRectangleFromTexture(
        src, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXTexture*>(this)->CopyRectangleFromTexture(
        src, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
#endif
  default:
    return;
  }
}

void AbstractTexture::ResolveFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& rect, u32 layer, u32 level)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXTexture*>(this)->ResolveFromTexture(src, rect, layer, level);
    return;
#endif
  default:
    return;
  }
}

void AbstractTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                           size_t buffer_size, u32 layer)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullTexture*>(this)->Load(level, width, height, row_length, buffer,
                                                buffer_size, layer);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWTexture*>(this)->Load(level, width, height, row_length, buffer, buffer_size,
                                            layer);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLTexture*>(this)->Load(level, width, height, row_length, buffer, buffer_size,
                                              layer);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKTexture*>(this)->Load(level, width, height, row_length, buffer,
                                                buffer_size, layer);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXTexture*>(this)->Load(level, width, height, row_length, buffer, buffer_size,
                                              layer);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXTexture*>(this)->Load(level, width, height, row_length, buffer, buffer_size,
                                              layer);
    return;
#endif
  default:
    return;
  }
}

u8* AbstractTexture::LendLoad(u32 level, u32 width, u32 height, u32 row_length, size_t size,
                              u32 layer)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLTexture*>(this)->LendLoad(level, width, height, row_length, size,
                                                         layer);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKTexture*>(this)->LendLoad(level, width, height, row_length, size,
                                                           layer);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::DXTexture*>(this)->LendLoad(level, width, height, row_length, size,
                                                         layer);
#endif
  default:
    return nullptr;
  }
}

void AbstractTexture::CommitLoad()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLTexture*>(this)->CommitLoad();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKTexture*>(this)->CommitLoad();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXTexture*>(this)->CommitLoad();
    return;
#endif
  default:
    return;
  }
}

void AbstractTexture::FinishedRendering()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKTexture*>(this)->FinishedRendering();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXTexture*>(this)->FinishedRendering();
    return;
#endif
  default:
    return;
  }
}

/* staging textures */

void std::default_delete<AbstractStagingTexture>::operator()(AbstractStagingTexture* object) const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    delete static_cast<Null::NullStagingTexture*>(object);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::SWStagingTexture*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLStagingTexture*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKStagingTexture*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::DXStagingTexture*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::DXStagingTexture*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

void AbstractStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                             const MathUtil::Rectangle<int>& src_rect,
                                             u32 src_layer, u32 src_level,
                                             const MathUtil::Rectangle<int>& dst_rect)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer,
                                                                  src_level, dst_rect);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer, src_level,
                                                              dst_rect);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer, src_level,
                                                                dst_rect);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer,
                                                                  src_level, dst_rect);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer, src_level,
                                                                dst_rect);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXStagingTexture*>(this)->CopyFromTexture(src, src_rect, src_layer, src_level,
                                                                dst_rect);
    return;
#endif
  default:
    return;
  }
}

void AbstractStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect,
                                           AbstractTexture* dst,
                                           const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                           u32 dst_level)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                                dst_level);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                            dst_level);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                              dst_level);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                                dst_level);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                              dst_level);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXStagingTexture*>(this)->CopyToTexture(src_rect, dst, dst_rect, dst_layer,
                                                              dst_level);
    return;
#endif
  default:
    return;
  }
}

bool AbstractStagingTexture::Map()
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullStagingTexture*>(this)->Map();
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWStagingTexture*>(this)->Map();
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLStagingTexture*>(this)->Map();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKStagingTexture*>(this)->Map();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::DXStagingTexture*>(this)->Map();
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::DXStagingTexture*>(this)->Map();
#endif
  default:
    return false;
  }
}

void AbstractStagingTexture::Unmap()
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullStagingTexture*>(this)->Unmap();
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWStagingTexture*>(this)->Unmap();
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLStagingTexture*>(this)->Unmap();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKStagingTexture*>(this)->Unmap();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXStagingTexture*>(this)->Unmap();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXStagingTexture*>(this)->Unmap();
    return;
#endif
  default:
    return;
  }
}

void AbstractStagingTexture::Flush()
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::NullStagingTexture*>(this)->Flush();
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWStagingTexture*>(this)->Flush();
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLStagingTexture*>(this)->Flush();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKStagingTexture*>(this)->Flush();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::DXStagingTexture*>(this)->Flush();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::DXStagingTexture*>(this)->Flush();
    return;
#endif
  default:
    return;
  }
}
