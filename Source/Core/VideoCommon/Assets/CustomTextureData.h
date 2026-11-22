// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include <retro_atomic.h>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon
{
// Shared between an asset and the texture cache: each holder keeps a reference
class CustomTextureData
{
public:
  void AddRef() { retro_atomic_fetch_add_int(&m_refs, 1); }
  // Deletes the data once the last reference goes
  void Release()
  {
    if (retro_atomic_fetch_sub_int(&m_refs, 1) == 1)
      delete this;
  }

  struct ArraySlice
  {
    struct Level
    {
      Common::UniqueBuffer<u8> data;
      AbstractTextureFormat format = AbstractTextureFormat::RGBA8;
      u32 width = 0;
      u32 height = 0;
      u32 row_length = 0;
    };
    std::vector<Level> m_levels;
  };
  std::vector<ArraySlice> m_slices;

private:
  retro_atomic_int_t m_refs{1};
};

bool LoadDDSTexture(CustomTextureData* texture, const std::string& filename);
bool LoadDDSTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename,
                    u32 mip_level);
bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename);
bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, const u8* buffer, size_t size);
}  // namespace VideoCommon
