// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d12.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/NativeVertexFormat.h"

enum class ShaderAttrib : u32;

namespace DX12
{
class DXVertexFormat final : public NativeVertexFormat
{
public:
  static const u32 MAX_VERTEX_ATTRIBUTES = 16;

  DXVertexFormat(const PortableVertexDeclaration& vtx_decl);

  // Passed to pipeline state creation
  void GetInputLayoutDesc(D3D12_INPUT_LAYOUT_DESC* desc) const;

private:
  void AddAttribute(const char* semantic_name, ShaderAttrib semantic_index, u32 slot,
                    DXGI_FORMAT format, u32 offset);
  void MapAttributes();

  D3D12_INPUT_ELEMENT_DESC m_attribute_descriptions[MAX_VERTEX_ATTRIBUTES] = {};
  u32 m_num_attributes = 0;
};
}  // namespace DX12
