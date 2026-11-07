// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include <retro_atomic.h>

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexManagerBase.h"

enum class ShaderAttrib : u32;

namespace DX11
{
class D3DVertexFormat : public NativeVertexFormat
{
public:
  D3DVertexFormat(const PortableVertexDeclaration& vtx_decl);
  ~D3DVertexFormat() override;
  ID3D11InputLayout* GetInputLayout(const void* vs_bytecode, size_t vs_bytecode_size);

private:
  void AddAttribute(const AttributeFormat& format, ShaderAttrib semantic_index);

  std::array<D3D11_INPUT_ELEMENT_DESC, 32> m_elems{};
  UINT m_num_elems = 0;

  retro_atomic_ptr_t m_layout{};
};

class VertexManager final : public VertexManagerBase
{
  /* called by the base class as this backend */
  friend class ::VertexManagerBase;

public:
  VertexManager();
  ~VertexManager();

  bool Initialize();

  void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size);
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                         u32* out_offset);
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format, u32* out_offset,
                         const void* palette_data, u32 palette_size,
                         TexelBufferFormat palette_format, u32* out_palette_offset);

protected:
  void ResetBuffer(u32 vertex_stride);
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index);
  void UploadUniforms();

private:
  bool MapTexelBuffer(u32 required_size, D3D11_MAPPED_SUBRESOURCE& sr);
  void UnmapBuffers();

  /* A batch's vertices and indices are decoded straight into these, mapped
   * from the batch's start to its commit. */
  ComPtr<ID3D11Buffer> m_vertex_buffer;
  ComPtr<ID3D11Buffer> m_index_buffer;
  u32 m_vertex_cursor = 0;
  u32 m_index_cursor = 0;
  bool m_buffers_mapped = false;

  ComPtr<ID3D11Buffer> m_vertex_constant_buffer = nullptr;
  ComPtr<ID3D11Buffer> m_geometry_constant_buffer = nullptr;
  ComPtr<ID3D11Buffer> m_pixel_constant_buffer = nullptr;


  ComPtr<ID3D11Buffer> m_texel_buffer = nullptr;
  std::array<ComPtr<ID3D11ShaderResourceView>, NUM_TEXEL_BUFFER_FORMATS> m_texel_buffer_views;
  u32 m_texel_buffer_offset = 0;
};

}  // namespace DX11
