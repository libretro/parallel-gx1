// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace VideoCommon
{
class CustomTextureData;

// This class provides functionality to load
// specific data (like textures).  Where this data
// is loaded from is implementation defined
class CustomAssetLibrary
{
public:
  // The AssetID is a unique identifier for a particular asset
  using AssetID = std::string;

  struct LoadInfo
  {
    std::size_t bytes_loaded = 0;
  };

  virtual ~CustomAssetLibrary() = default;

  // Loads a texture, if there are no levels, bytes loaded will be empty
  virtual LoadInfo LoadTexture(const AssetID& asset_id, CustomTextureData* data) = 0;
};
}  // namespace VideoCommon
