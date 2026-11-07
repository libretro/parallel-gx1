// SPDX-License-Identifier: GPL-2.0-or-later

/* VertexManagerBase calls into the vertex manager of the backend in use
 * (g_video_backend_kind); each backend's is final, so these are direct
 * calls, and what a backend does not do itself falls to the defaults. */

#include "VideoCommon/VertexManagerBase.h"

#include "Common/CommonTypes.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/VideoBackendKind.h"

#include "VideoBackends/Null/NullVertexManager.h"
#include "VideoBackends/Software/SWVertexLoader.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/OGLVertexManager.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VKVertexManager.h"
#endif
#ifdef _WIN32
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3D12/D3D12VertexManager.h"
#endif

void std::default_delete<VertexManagerBase>::operator()(VertexManagerBase* object) const
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    delete static_cast<Null::VertexManager*>(object);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    delete static_cast<SWVertexLoader*>(object);
    return;
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    delete static_cast<OGL::VertexManager*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Vulkan::VertexManager*>(object);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<DX11::VertexManager*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<DX12::VertexManager*>(object);
    return;
#endif
  default:
    delete object;
    return;
  }
}

bool VertexManagerBase::Initialize()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::VertexManager*>(this)->Initialize();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VertexManager*>(this)->Initialize();
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::VertexManager*>(this)->Initialize();
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::VertexManager*>(this)->Initialize();
#endif
  default:
    return BaseInitialize();
  }
}

DataReader VertexManagerBase::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive,
                                                       u32 count, u32 stride, bool cullall)
{
  if (g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
  {
    return static_cast<SWVertexLoader*>(this)->PrepareForAdditionalData(primitive, count, stride,
                                                                        cullall);
  }
  return BasePrepareForAdditionalData(primitive, count, stride, cullall);
}

void VertexManagerBase::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::VertexManager*>(this)->UploadUtilityUniforms(uniforms, uniforms_size);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VertexManager*>(this)->UploadUtilityUniforms(uniforms, uniforms_size);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::VertexManager*>(this)->UploadUtilityUniforms(uniforms, uniforms_size);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::VertexManager*>(this)->UploadUtilityUniforms(uniforms, uniforms_size);
    return;
#endif
  default:
    return;
  }
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size,
                                          TexelBufferFormat format, u32* out_offset)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::VertexManager*>(this)->UploadTexelBuffer(data, data_size, format,
                                                                     out_offset);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VertexManager*>(this)->UploadTexelBuffer(data, data_size, format,
                                                                        out_offset);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::VertexManager*>(this)->UploadTexelBuffer(data, data_size, format,
                                                                      out_offset);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::VertexManager*>(this)->UploadTexelBuffer(data, data_size, format,
                                                                      out_offset);
#endif
  default:
    return false;
  }
}

bool VertexManagerBase::UploadTexelBuffer(const void* data, u32 data_size,
                                          TexelBufferFormat format, u32* out_offset,
                                          const void* palette_data, u32 palette_size,
                                          TexelBufferFormat palette_format,
                                          u32* out_palette_offset)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    return static_cast<OGL::VertexManager*>(this)->UploadTexelBuffer(
        data, data_size, format, out_offset, palette_data, palette_size, palette_format,
        out_palette_offset);
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<Vulkan::VertexManager*>(this)->UploadTexelBuffer(
        data, data_size, format, out_offset, palette_data, palette_size, palette_format,
        out_palette_offset);
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<DX11::VertexManager*>(this)->UploadTexelBuffer(
        data, data_size, format, out_offset, palette_data, palette_size, palette_format,
        out_palette_offset);
  case VIDEO_BACKEND_D3D12:
    return static_cast<DX12::VertexManager*>(this)->UploadTexelBuffer(
        data, data_size, format, out_offset, palette_data, palette_size, palette_format,
        out_palette_offset);
#endif
  default:
    return false;
  }
}

void VertexManagerBase::ResetBuffer(u32 vertex_stride)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::VertexManager*>(this)->ResetBuffer(vertex_stride);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VertexManager*>(this)->ResetBuffer(vertex_stride);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::VertexManager*>(this)->ResetBuffer(vertex_stride);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::VertexManager*>(this)->ResetBuffer(vertex_stride);
    return;
#endif
  default:
    BaseResetBuffer(vertex_stride);
    return;
  }
}

void VertexManagerBase::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                     u32* out_base_vertex, u32* out_base_index)
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::VertexManager*>(this)->CommitBuffer(num_vertices, vertex_stride, num_indices,
                                                         out_base_vertex, out_base_index);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VertexManager*>(this)->CommitBuffer(
        num_vertices, vertex_stride, num_indices, out_base_vertex, out_base_index);
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::VertexManager*>(this)->CommitBuffer(num_vertices, vertex_stride, num_indices,
                                                          out_base_vertex, out_base_index);
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::VertexManager*>(this)->CommitBuffer(num_vertices, vertex_stride, num_indices,
                                                          out_base_vertex, out_base_index);
    return;
#endif
  default:
    *out_base_vertex = 0;
    *out_base_index = 0;
    return;
  }
}

void VertexManagerBase::UploadUniforms()
{
  switch (g_video_backend_kind)
  {
#ifdef HAS_OPENGL
  case VIDEO_BACKEND_OGL:
    static_cast<OGL::VertexManager*>(this)->UploadUniforms();
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    static_cast<Vulkan::VertexManager*>(this)->UploadUniforms();
    return;
#endif
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    static_cast<DX11::VertexManager*>(this)->UploadUniforms();
    return;
  case VIDEO_BACKEND_D3D12:
    static_cast<DX12::VertexManager*>(this)->UploadUniforms();
    return;
#endif
  default:
    return;
  }
}

void VertexManagerBase::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  switch (g_video_backend_kind)
  {
  case VIDEO_BACKEND_NULL:
    static_cast<Null::VertexManager*>(this)->DrawCurrentBatch(base_index, num_indices,
                                                              base_vertex);
    return;
  case VIDEO_BACKEND_SOFTWARE:
    static_cast<SWVertexLoader*>(this)->DrawCurrentBatch(base_index, num_indices, base_vertex);
    return;
  default:
    BaseDrawCurrentBatch(base_index, num_indices, base_vertex);
    return;
  }
}
