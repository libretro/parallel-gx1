// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DVertexManager.h"

#include <d3d11_4.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "Core/System.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
static ComPtr<ID3D11Buffer> AllocateConstantBuffer(u32 size)
{
  const u32 cbsize = Common::AlignUp(size, 16u);  // must be a multiple of 16
  const CD3D11_BUFFER_DESC cbdesc(cbsize, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC,
                                  D3D11_CPU_ACCESS_WRITE);
  ComPtr<ID3D11Buffer> cbuf;
  const HRESULT hr = D3D::device->CreateBuffer(&cbdesc, nullptr, &cbuf);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create shader constant buffer (size={}): {}", cbsize,
             DX11HRWrap(hr));
  if (FAILED(hr))
    return nullptr;

  D3DCommon::SetDebugObjectName(cbuf.Get(), "constant buffer");
  return cbuf;
}

static void UpdateConstantBuffer(ID3D11Buffer* const buffer, const void* data, u32 data_size)
{
  D3D11_MAPPED_SUBRESOURCE map;
  D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
  memcpy(map.pData, data, data_size);
  D3D::context->Unmap(buffer, 0);

  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
}

static ComPtr<ID3D11ShaderResourceView>
CreateTexelBufferView(ID3D11Buffer* buffer, TexelBufferFormat format, DXGI_FORMAT srv_format)
{
  ComPtr<ID3D11ShaderResourceView> srv;
  CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(
#ifdef __MINGW32__
    D3D11_SRV_DIMENSION_BUFFER,
#else
    buffer,
#endif
    srv_format, 0,
    VertexManager::TEXEL_STREAM_BUFFER_SIZE /
    VertexManager::GetTexelBufferElementSize(format)
#ifdef __MINGW32__
    , 0, -1);
#else
  );
#endif
  HRESULT hr = D3D::device->CreateShaderResourceView(buffer, &srv_desc, &srv);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create SRV for texel buffer: {}", DX11HRWrap(hr));
  return srv;
}

VertexManager::VertexManager() = default;

VertexManager::~VertexManager()
{
  UnmapBuffers();
}

bool VertexManager::Initialize()
{
  if (!BaseInitialize())
    return false;

  const CD3D11_BUFFER_DESC vertex_desc(VERTEX_STREAM_BUFFER_SIZE, D3D11_BIND_VERTEX_BUFFER,
                                       D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  const CD3D11_BUFFER_DESC index_desc(INDEX_STREAM_BUFFER_SIZE, D3D11_BIND_INDEX_BUFFER,
                                      D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  HRESULT hr = D3D::device->CreateBuffer(&vertex_desc, nullptr, &m_vertex_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create vertex buffer: {}", DX11HRWrap(hr));
  hr = D3D::device->CreateBuffer(&index_desc, nullptr, &m_index_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create index buffer: {}", DX11HRWrap(hr));
  if (!m_vertex_buffer || !m_index_buffer)
    return false;
  D3DCommon::SetDebugObjectName(m_vertex_buffer.Get(), "VertexManager vertex buffer");
  D3DCommon::SetDebugObjectName(m_index_buffer.Get(), "VertexManager index buffer");

  m_vertex_constant_buffer = AllocateConstantBuffer(sizeof(VertexShaderConstants));
  m_geometry_constant_buffer = AllocateConstantBuffer(sizeof(GeometryShaderConstants));
  m_pixel_constant_buffer = AllocateConstantBuffer(sizeof(PixelShaderConstants));
  if (!m_vertex_constant_buffer || !m_geometry_constant_buffer || !m_pixel_constant_buffer)
    return false;

  CD3D11_BUFFER_DESC texel_buf_desc(TEXEL_STREAM_BUFFER_SIZE, D3D11_BIND_SHADER_RESOURCE,
                                    D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  hr = D3D::device->CreateBuffer(&texel_buf_desc, nullptr, &m_texel_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating texel buffer failed: {}", DX11HRWrap(hr));
  if (!m_texel_buffer)
    return false;

  static constexpr std::array<std::pair<TexelBufferFormat, DXGI_FORMAT>, NUM_TEXEL_BUFFER_FORMATS>
      format_mapping = {{
          {TEXEL_BUFFER_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT},
          {TEXEL_BUFFER_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT},
          {TEXEL_BUFFER_FORMAT_RGBA8_UINT, DXGI_FORMAT_R8G8B8A8_UINT},
          {TEXEL_BUFFER_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT},
      }};
  for (const auto& it : format_mapping)
  {
    m_texel_buffer_views[it.first] =
        CreateTexelBufferView(m_texel_buffer.Get(), it.first, it.second);
    if (!m_texel_buffer_views[it.first])
      return false;
  }

  return true;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  // Just use the one buffer for all three.
  InvalidateConstants();
  UpdateConstantBuffer(m_vertex_constant_buffer.Get(), uniforms, uniforms_size);
  D3D::stateman->SetVertexConstants(m_vertex_constant_buffer.Get());
  D3D::stateman->SetGeometryConstants(m_vertex_constant_buffer.Get());
  D3D::stateman->SetPixelConstants(m_vertex_constant_buffer.Get());
}

bool VertexManager::MapTexelBuffer(u32 required_size, D3D11_MAPPED_SUBRESOURCE& sr)
{
  if ((m_texel_buffer_offset + required_size) > TEXEL_STREAM_BUFFER_SIZE)
  {
    // Restart buffer.
    HRESULT hr = D3D::context->Map(m_texel_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
    ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to map texel buffer: {}", DX11HRWrap(hr));
    if (FAILED(hr))
      return false;

    m_texel_buffer_offset = 0;
  }
  else
  {
    // Don't overwrite the earlier-used space.
    HRESULT hr = D3D::context->Map(m_texel_buffer.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &sr);
    ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to map texel buffer: {}", DX11HRWrap(hr));
    if (FAILED(hr))
      return false;
  }

  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  const u32 elem_size = GetTexelBufferElementSize(format);
  m_texel_buffer_offset = Common::AlignUp(m_texel_buffer_offset, elem_size);

  D3D11_MAPPED_SUBRESOURCE sr;
  if (!MapTexelBuffer(data_size, sr))
    return false;

  *out_offset = m_texel_buffer_offset / elem_size;
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset, data, data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  m_texel_buffer_offset += data_size;

  D3D::context->Unmap(m_texel_buffer.Get(), 0);
  D3D::stateman->SetTexture(0, m_texel_buffer_views[static_cast<size_t>(format)].Get());
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 elem_size = GetTexelBufferElementSize(format);
  const u32 palette_elem_size = GetTexelBufferElementSize(palette_format);
  const u32 reserve_size = data_size + palette_size + palette_elem_size;
  if (reserve_size > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  m_texel_buffer_offset = Common::AlignUp(m_texel_buffer_offset, elem_size);

  D3D11_MAPPED_SUBRESOURCE sr;
  if (!MapTexelBuffer(reserve_size, sr))
    return false;

  const u32 palette_byte_offset = Common::AlignUp(data_size, palette_elem_size);
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset, data, data_size);
  std::memcpy(static_cast<u8*>(sr.pData) + m_texel_buffer_offset + palette_byte_offset,
              palette_data, palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, palette_byte_offset + palette_size);
  *out_offset = m_texel_buffer_offset / elem_size;
  *out_palette_offset = (m_texel_buffer_offset + palette_byte_offset) / palette_elem_size;
  m_texel_buffer_offset += palette_byte_offset + palette_size;

  D3D::context->Unmap(m_texel_buffer.Get(), 0);
  D3D::stateman->SetTexture(0, m_texel_buffer_views[static_cast<size_t>(format)].Get());
  D3D::stateman->SetTexture(1, m_texel_buffer_views[static_cast<size_t>(palette_format)].Get());
  return true;
}

void VertexManager::UnmapBuffers()
{
  if (!m_buffers_mapped)
    return;
  D3D::context->Unmap(m_vertex_buffer.Get(), 0);
  D3D::context->Unmap(m_index_buffer.Get(), 0);
  m_buffers_mapped = false;
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  D3D11_MAPPED_SUBRESOURCE vertex_map;
  D3D11_MAPPED_SUBRESOURCE index_map;
  D3D11_MAP vertex_map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
  D3D11_MAP index_map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
  u32 vertex_cursor = m_vertex_cursor;
  u32 index_cursor = m_index_cursor;

  /* a batch dropped unsent still holds them */
  UnmapBuffers();
  D3D::stateman->UnbindInputBuffers();

  if (vertex_stride > 0 && (vertex_cursor % vertex_stride) != 0)
    vertex_cursor += vertex_stride - (vertex_cursor % vertex_stride);
  if (vertex_cursor + MAXVBUFFERSIZE > VERTEX_STREAM_BUFFER_SIZE)
  {
    vertex_cursor = 0;
    vertex_map_type = D3D11_MAP_WRITE_DISCARD;
  }
  if (index_cursor + MAXIBUFFERSIZE * sizeof(u16) > INDEX_STREAM_BUFFER_SIZE)
  {
    index_cursor = 0;
    index_map_type = D3D11_MAP_WRITE_DISCARD;
  }

  if (FAILED(D3D::context->Map(m_vertex_buffer.Get(), 0, vertex_map_type, 0, &vertex_map)))
  {
    BaseResetBuffer(vertex_stride);
    return;
  }
  if (FAILED(D3D::context->Map(m_index_buffer.Get(), 0, index_map_type, 0, &index_map)))
  {
    D3D::context->Unmap(m_vertex_buffer.Get(), 0);
    BaseResetBuffer(vertex_stride);
    return;
  }
  m_buffers_mapped = true;
  m_vertex_cursor = vertex_cursor;
  m_index_cursor = index_cursor;

  m_base_buffer_pointer = static_cast<u8*>(vertex_map.pData) + vertex_cursor;
  m_cur_buffer_pointer = m_base_buffer_pointer;
  m_end_buffer_pointer = m_base_buffer_pointer + MAXVBUFFERSIZE;
  m_index_generator.Start(
      reinterpret_cast<u16*>(static_cast<u8*>(index_map.pData) + index_cursor));
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  const u32 vertex_data_size = num_vertices * vertex_stride;
  const u32 index_data_size = num_indices * sizeof(u16);

  *out_base_vertex = vertex_stride > 0 ? (m_vertex_cursor / vertex_stride) : 0;
  *out_base_index = m_index_cursor / sizeof(u16);

  UnmapBuffers();
  m_vertex_cursor += vertex_data_size;
  m_index_cursor += index_data_size;

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vertex_data_size);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, index_data_size);

  D3D::stateman->SetVertexBuffer(m_vertex_buffer.Get(), vertex_stride, 0);
  D3D::stateman->SetIndexBuffer(m_index_buffer.Get());
}

void VertexManager::UploadUniforms()
{
  auto& system = Core::System::GetInstance();

  auto& vertex_shader_manager = system.GetVertexShaderManager();
  if (vertex_shader_manager.dirty)
  {
    UpdateConstantBuffer(m_vertex_constant_buffer.Get(), &vertex_shader_manager.constants,
                         sizeof(VertexShaderConstants));
    vertex_shader_manager.dirty = false;
  }

  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  if (geometry_shader_manager.dirty)
  {
    UpdateConstantBuffer(m_geometry_constant_buffer.Get(), &geometry_shader_manager.constants,
                         sizeof(GeometryShaderConstants));
    geometry_shader_manager.dirty = false;
  }

  auto& pixel_shader_manager = system.GetPixelShaderManager();
  if (pixel_shader_manager.dirty)
  {
    UpdateConstantBuffer(m_pixel_constant_buffer.Get(), &pixel_shader_manager.constants,
                         sizeof(PixelShaderConstants));
    pixel_shader_manager.dirty = false;
  }

  D3D::stateman->SetPixelConstants(
      m_pixel_constant_buffer.Get(),
      g_ActiveConfig.bEnablePixelLighting ? m_vertex_constant_buffer.Get() : nullptr);
  D3D::stateman->SetVertexConstants(m_vertex_constant_buffer.Get());
  D3D::stateman->SetGeometryConstants(m_geometry_constant_buffer.Get());
}
}  // namespace DX11
