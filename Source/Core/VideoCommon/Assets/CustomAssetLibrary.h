// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace VideoCommon
{
class DirectFilesystemAssetLibrary;

// What loading assets (like textures) is spoken in; the one library that loads them is
// DirectFilesystemAssetLibrary, from the file system.
class CustomAssetLibrary
{
public:
  // The AssetID is a unique identifier for a particular asset
  using AssetID = std::string;

  struct LoadInfo
  {
    std::size_t bytes_loaded = 0;
  };
};
}  // namespace VideoCommon
