// SPDX-License-Identifier: GPL-2.0-or-later

/* AbstractGfx calls into the Gfx of the backend in use
 * (g_video_backend_kind); each backend's Gfx is final, so these are direct
 * calls, and what a backend does not do itself falls to the defaults. */

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/StutterStats.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoBackendKind.h"

#include "VideoBackends/Null/NullGfx.h"
#include "VideoBackends/Software/SWGfx.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLGfx.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VKGfx.h"
#endif
#ifdef _WIN32
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#endif

void AbstractGfxDelete(AbstractGfx* object)
{
  if (!object)
    return;
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    delete static_cast<Null::NullGfx*>(object);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SW::SWGfx*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::OGLGfx*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VKGfx*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::Gfx*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::Gfx*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

bool AbstractGfx::IsHeadless() const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<const Null::NullGfx*>(this)->IsHeadless();
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<const SW::SWGfx*>(this)->IsHeadless();
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::OGLGfx*>(this)->IsHeadless();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Vulkan::VKGfx*>(this)->IsHeadless();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const DX11::Gfx*>(this)->IsHeadless();
  case VIDEO_BACKEND_D3D12:
    return static_cast<const DX12::Gfx*>(this)->IsHeadless();
#endif
  default:
    return true;
  }
}

bool AbstractGfx::SupportsUtilityDrawing() const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<const Null::NullGfx*>(this)->SupportsUtilityDrawing();
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<const SW::SWGfx*>(this)->SupportsUtilityDrawing();
  default:
    return true;
  }
}

void AbstractGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetPipeline(pipeline);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetPipeline(pipeline);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetPipeline(pipeline);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetPipeline(pipeline);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWGfx*>(this)->SetScissorRect(rc);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetScissorRect(rc);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetScissorRect(rc);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetScissorRect(rc);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetScissorRect(rc);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetTexture(index, texture);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetTexture(index, texture);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetTexture(index, texture);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetTexture(index, texture);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetSamplerState(index, state);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetSamplerState(index, state);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetSamplerState(index, state);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetSamplerState(index, state);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetComputeImageTexture(index, texture, read, write);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetComputeImageTexture(index, texture, read, write);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetComputeImageTexture(index, texture, read, write);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetComputeImageTexture(index, texture, read, write);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::UnbindTexture(const AbstractTexture* texture)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->UnbindTexture(texture);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->UnbindTexture(texture);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->UnbindTexture(texture);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->UnbindTexture(texture);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                              float far_depth)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetViewport(x, y, width, height, near_depth, far_depth);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetViewport(x, y, width, height, near_depth, far_depth);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetViewport(x, y, width, height, near_depth, far_depth);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetViewport(x, y, width, height, near_depth, far_depth);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::BeginUtilityDrawing()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->BeginUtilityDrawing();
    return;
#endif
  default:
    BaseBeginUtilityDrawing();
    return;
  }
}

void AbstractGfx::EndUtilityDrawing()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->EndUtilityDrawing();
    return;
#endif
  default:
    BaseEndUtilityDrawing();
    return;
  }
}

AbstractTexture* AbstractGfx::CreateTexture(const TextureConfig& config,
                                                            std::string_view name)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullGfx*>(this)->CreateTexture(config, name);
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->CreateTexture(config, name);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->CreateTexture(config, name);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->CreateTexture(config, name);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->CreateTexture(config, name);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->CreateTexture(config, name);
#endif
  default:
    return nullptr;
  }
}

AbstractStagingTexture*
AbstractGfx::CreateStagingTexture(StagingTextureType type, const TextureConfig& config)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullGfx*>(this)->CreateStagingTexture(type, config);
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->CreateStagingTexture(type, config);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->CreateStagingTexture(type, config);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->CreateStagingTexture(type, config);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->CreateStagingTexture(type, config);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->CreateStagingTexture(type, config);
#endif
  default:
    return nullptr;
  }
}

AbstractFramebuffer*
AbstractGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                               std::vector<AbstractTexture*> additional_color_attachments)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullGfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->CreateFramebuffer(
        color_attachment, depth_attachment, std::move(additional_color_attachments));
#endif
  default:
    return nullptr;
  }
}

void AbstractGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetFramebuffer(framebuffer);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetFramebuffer(framebuffer);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetFramebuffer(framebuffer);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetFramebuffer(framebuffer);
    return;
#endif
  default:
    BaseSetFramebuffer(framebuffer);
    return;
  }
}

void AbstractGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetAndDiscardFramebuffer(framebuffer);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetAndDiscardFramebuffer(framebuffer);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetAndDiscardFramebuffer(framebuffer);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetAndDiscardFramebuffer(framebuffer);
    return;
#endif
  default:
    BaseSetAndDiscardFramebuffer(framebuffer);
    return;
  }
}

void AbstractGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                         const ClearColor& color_value, float depth_value)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SetAndClearFramebuffer(framebuffer, color_value, depth_value);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->SetAndClearFramebuffer(framebuffer, color_value,
                                                              depth_value);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->SetAndClearFramebuffer(framebuffer, color_value, depth_value);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->SetAndClearFramebuffer(framebuffer, color_value, depth_value);
    return;
#endif
  default:
    BaseSetAndClearFramebuffer(framebuffer, color_value, depth_value);
    return;
  }
}

void AbstractGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                              bool alphaEnable, bool zEnable, u32 color, u32 z)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWGfx*>(this)->ClearRegion(target_rc, colorEnable, alphaEnable, zEnable, color,
                                               z);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->ClearRegion(target_rc, colorEnable, alphaEnable, zEnable,
                                                 color, z);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->ClearRegion(target_rc, colorEnable, alphaEnable, zEnable,
                                                   color, z);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->ClearRegion(target_rc, colorEnable, alphaEnable, zEnable, color,
                                               z);
    return;
#endif
  default:
    BaseClearRegion(target_rc, colorEnable, alphaEnable, zEnable, color, z);
    return;
  }
}

void AbstractGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->Draw(base_vertex, num_vertices);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->Draw(base_vertex, num_vertices);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->Draw(base_vertex, num_vertices);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->Draw(base_vertex, num_vertices);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->DrawIndexed(base_index, num_indices, base_vertex);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->DrawIndexed(base_index, num_indices, base_vertex);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->DrawIndexed(base_index, num_indices, base_vertex);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->DrawIndexed(base_index, num_indices, base_vertex);
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x,
                                        u32 groupsize_y, u32 groupsize_z, u32 groups_x,
                                        u32 groups_y, u32 groups_z)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->DispatchComputeShader(
        shader, groupsize_x, groupsize_y, groupsize_z, groups_x, groups_y, groups_z);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->DispatchComputeShader(
        shader, groupsize_x, groupsize_y, groupsize_z, groups_x, groups_y, groups_z);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->DispatchComputeShader(shader, groupsize_x, groupsize_y,
                                                         groupsize_z, groups_x, groups_y, groups_z);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->DispatchComputeShader(shader, groupsize_x, groupsize_y,
                                                         groupsize_z, groups_x, groups_y, groups_z);
    return;
#endif
  default:
    return;
  }
}

bool AbstractGfx::BindBackbuffer(const ClearColor& clear_color)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->BindBackbuffer(clear_color);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->BindBackbuffer(clear_color);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->BindBackbuffer(clear_color);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->BindBackbuffer(clear_color);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->BindBackbuffer(clear_color);
#endif
  default:
    return true;
  }
}

void AbstractGfx::PresentBackbuffer()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->PresentBackbuffer();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->PresentBackbuffer();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->PresentBackbuffer();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->PresentBackbuffer();
    return;
#endif
  default:
    return;
  }
}

bool AbstractGfx::PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->PresentTexture(texture, width, height);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->PresentTexture(texture, width, height);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->PresentTexture(texture, width, height);
#endif
  default:
    return false;
  }
}

bool AbstractGfx::IsTextureHeld(const AbstractTexture* texture)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->IsTextureHeld(texture);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->IsTextureHeld(texture);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->IsTextureHeld(texture);
#endif
  default:
    return false;
  }
}

AbstractShader* AbstractGfx::CreateShaderFromSource(ShaderStage stage,
                                                                    ShaderCode* source,
                                                                    std::string_view name)
{
  AbstractShader* shader = nullptr;
  STUTTER_MARK(t);
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    shader = static_cast<Null::NullGfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
  case VIDEO_BACKEND_SOFTWARE:
    shader = static_cast<SW::SWGfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    shader = static_cast<OGL::OGLGfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    shader = static_cast<Vulkan::VKGfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    shader = static_cast<DX11::Gfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
  case VIDEO_BACKEND_D3D12:
    shader = static_cast<DX12::Gfx*>(this)->CreateShaderFromSource(stage, source, name);
    break;
#endif
  default:
    break;
  }
  STUTTER_SINCE(STUTTER_SHADER_CREATE, t);
  return shader;
}

AbstractShader* AbstractGfx::CreateShaderFromBinary(ShaderStage stage,
                                                                    const void* data, size_t length,
                                                                    std::string_view name)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullGfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->CreateShaderFromBinary(stage, data, length, name);
#endif
  default:
    return nullptr;
  }
}

NativeVertexFormat*
AbstractGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<Null::NullGfx*>(this)->CreateNativeVertexFormat(vtx_decl);
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<SW::SWGfx*>(this)->CreateNativeVertexFormat(vtx_decl);
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::OGLGfx*>(this)->CreateNativeVertexFormat(vtx_decl);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VKGfx*>(this)->CreateNativeVertexFormat(vtx_decl);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::Gfx*>(this)->CreateNativeVertexFormat(vtx_decl);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::Gfx*>(this)->CreateNativeVertexFormat(vtx_decl);
#endif
  default:
    return nullptr;
  }
}

AbstractPipeline* AbstractGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                              const void* cache_data,
                                                              size_t cache_data_length)
{
  AbstractPipeline* pipeline = nullptr;
  STUTTER_MARK(t);
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    pipeline = static_cast<Null::NullGfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
  case VIDEO_BACKEND_SOFTWARE:
    pipeline = static_cast<SW::SWGfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    pipeline = static_cast<OGL::OGLGfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    pipeline = static_cast<Vulkan::VKGfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    pipeline = static_cast<DX11::Gfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
  case VIDEO_BACKEND_D3D12:
    pipeline = static_cast<DX12::Gfx*>(this)->CreatePipeline(config, cache_data, cache_data_length);
    break;
#endif
  default:
    break;
  }
  STUTTER_SINCE(STUTTER_PIPELINE_CREATE, t);
  return pipeline;
}

void AbstractGfx::ScaleTexture(AbstractFramebuffer* dst_framebuffer,
                               const MathUtil::Rectangle<int>& dst_rect,
                               const AbstractTexture* src_texture,
                               const MathUtil::Rectangle<int>& src_rect)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWGfx*>(this)->ScaleTexture(dst_framebuffer, dst_rect, src_texture, src_rect);
    return;
  default:
    BaseScaleTexture(dst_framebuffer, dst_rect, src_texture, src_rect);
    return;
  }
}

void AbstractGfx::Flush()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->Flush();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->Flush();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->Flush();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->Flush();
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::WaitForGPUIdle()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->WaitForGPUIdle();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->WaitForGPUIdle();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::Gfx*>(this)->WaitForGPUIdle();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->WaitForGPUIdle();
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SelectLeftBuffer()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SelectLeftBuffer();
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SelectRightBuffer()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SelectRightBuffer();
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::SelectMainBuffer()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->SelectMainBuffer();
    return;
#endif
  default:
    return;
  }
}

void AbstractGfx::ShowImage(const AbstractTexture* source_texture,
                            const MathUtil::Rectangle<int>& source_rc)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SW::SWGfx*>(this)->ShowImage(source_texture, source_rc);
    return;
  default:
    return;
  }
}

bool AbstractGfx::CompilesShadersOnItsOwnThreads() const
{
#ifdef HAS_OPENGL
  if (g_video_backend_kind == VIDEO_BACKEND_OGL)
    return OGL::g_ogl_config.bSupportsParallelShaderCompile;
#endif
  return false;
}

void AbstractGfx::OnConfigChanged(u32 changed_bits)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::OGLGfx*>(this)->OnConfigChanged(changed_bits);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VKGfx*>(this)->OnConfigChanged(changed_bits);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::Gfx*>(this)->OnConfigChanged(changed_bits);
    return;
#endif
  default:
    BaseOnConfigChanged(changed_bits);
    return;
  }
}

SurfaceInfo AbstractGfx::GetSurfaceInfo() const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    return static_cast<const Null::NullGfx*>(this)->GetSurfaceInfo();
  case VIDEO_BACKEND_SOFTWARE:
    return static_cast<const SW::SWGfx*>(this)->GetSurfaceInfo();
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<const OGL::OGLGfx*>(this)->GetSurfaceInfo();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Vulkan::VKGfx*>(this)->GetSurfaceInfo();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const DX11::Gfx*>(this)->GetSurfaceInfo();
  case VIDEO_BACKEND_D3D12:
    return static_cast<const DX12::Gfx*>(this)->GetSurfaceInfo();
#endif
  default:
    return {};
  }
}
