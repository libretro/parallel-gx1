// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/OpcodeDecoding.h"

class CPUCull
{
public:
  ~CPUCull();
  void Init();
  bool AreAllVerticesCulled(VertexLoaderBase* loader, OpcodeDecoder::Primitive primitive,
                            const u8* src, u32 count);

  struct alignas(16) TransformedVertex
  {
    float x, y, z, w;
  };

  using TransformFunction = void (*)(void*, const void*, u32, int);
  using CullFunction = bool (*)(const CPUCull::TransformedVertex*, int);

private:
  // room for the most vertices a draw has (its count is 16 bits), made in Init
  static constexpr u32 MAX_VERTICES = 65536;
  TransformedVertex* m_transform_buffer = nullptr;
  TransformFunction m_transform_table[2][2]{};
  Common::EnumMap<Common::EnumMap<CullFunction, CullMode::All>,
                  OpcodeDecoder::Primitive::GX_DRAW_TRIANGLE_FAN>
      m_cull_table{};
};
