// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexLoaderBase.h"

#include <bit>

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"
#include "VideoCommon/VideoConfig.h"

#ifdef _M_X86_64
#include "VideoCommon/VertexLoaderX64.h"
#elif defined(_M_ARM_64)
#include "VideoCommon/VertexLoaderARM64.h"
#endif

u32 VertexLoaderBase::GetVertexSize(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  u32 size = 0;

  // Each enabled TexMatIdx adds one byte, as does PosMatIdx
  size += std::popcount(vtx_desc.low.Hex & 0x1FF);

  const u32 pos_size = VertexLoader_Position::GetSize(vtx_desc.low.Position, vtx_attr.g0.PosFormat,
                                                      vtx_attr.g0.PosElements);
  size += pos_size;
  const u32 norm_size =
      VertexLoader_Normal::GetSize(vtx_desc.low.Normal, vtx_attr.g0.NormalFormat,
                                   vtx_attr.g0.NormalElements, vtx_attr.g0.NormalIndex3);
  size += norm_size;
  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    const u32 color_size =
        VertexLoader_Color::GetSize(vtx_desc.low.Color[i], vtx_attr.GetColorFormat(i));
    size += color_size;
  }
  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    const u32 tc_size = VertexLoader_TextCoord::GetSize(
        vtx_desc.high.TexCoord[i], vtx_attr.GetTexFormat(i), vtx_attr.GetTexElements(i));
    size += tc_size;
  }

  return size;
}

u32 VertexLoaderBase::GetVertexComponents(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  u32 components = 0;
  if (vtx_desc.low.PosMatIdx)
    components |= VB_HAS_POSMTXIDX;
  for (u32 i = 0; i < vtx_desc.low.TexMatIdx.Size(); i++)
  {
    if (vtx_desc.low.TexMatIdx[i])
      components |= VB_HAS_TEXMTXIDX0 << i;
  }
  // Vertices always have positions; thus there is no VB_HAS_POS as it would always be set
  if (vtx_desc.low.Normal != VertexComponentFormat::NotPresent)
  {
    components |= VB_HAS_NORMAL;
    if (vtx_attr.g0.NormalElements == NormalComponentCount::NTB)
      components |= VB_HAS_TANGENT | VB_HAS_BINORMAL;
  }
  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    if (vtx_desc.low.Color[i] != VertexComponentFormat::NotPresent)
      components |= VB_HAS_COL0 << i;
  }
  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    if (vtx_desc.high.TexCoord[i] != VertexComponentFormat::NotPresent)
      components |= VB_HAS_UV0 << i;
  }
  return components;
}

VertexLoaderBase* VertexLoaderBase::CreateVertexLoader(const TVtxDesc& vtx_desc,
                                                       const VAT& vtx_attr)
{
#if defined(_M_X86_64)
  if (g_ActiveConfig.vertex_loader_type != VertexLoaderType::Software)
    return new VertexLoaderX64(vtx_desc, vtx_attr);
#elif defined(_M_ARM_64)
  if (g_ActiveConfig.vertex_loader_type != VertexLoaderType::Software)
    return new VertexLoaderARM64(vtx_desc, vtx_attr);
#endif
  return new VertexLoader(vtx_desc, vtx_attr);
}

int VertexLoaderBase::RunVertices(const u8* src, u8* dst, int count)
{
#if defined(_M_X86_64)
  if (m_jit)
    return static_cast<VertexLoaderX64*>(this)->RunVertices(src, dst, count);
#elif defined(_M_ARM_64)
  if (m_jit)
    return static_cast<VertexLoaderARM64*>(this)->RunVertices(src, dst, count);
#endif
  return static_cast<VertexLoader*>(this)->RunVertices(src, dst, count);
}

void VertexLoaderDelete(VertexLoaderBase* loader)
{
  if (!loader)
    return;
#if defined(_M_X86_64)
  if (loader->m_jit)
  {
    delete static_cast<VertexLoaderX64*>(loader);
    return;
  }
#elif defined(_M_ARM_64)
  if (loader->m_jit)
  {
    delete static_cast<VertexLoaderARM64*>(loader);
    return;
  }
#endif
  delete static_cast<VertexLoader*>(loader);
}
