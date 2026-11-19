// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
class CustomAssetCache;

// A texture asset's data as the texture cache takes it: loaded on request, kept until the
// asset reloads or fails to.
class TextureDataResource
{
public:
  TextureDataResource(const CustomAssetLibrary::AssetID& asset_id,
                      std::shared_ptr<DirectFilesystemAssetLibrary> library,
                      CustomAssetCache* asset_cache);

  // Marks the asset in use and takes its data once it has loaded.
  void Process();

  std::shared_ptr<CustomTextureData> GetData() const { return m_current_texture_data; }
  TimePoint GetLoadTime() const { return m_current_time; }

  // Asset cache: the asset finished loading, or failed to.
  void NotifyAssetLoadSuccess() { m_state = State::Loading; }
  void NotifyAssetLoadFailed() { m_state = State::Error; }

private:
  enum class State
  {
    Loading,
    Ready,
    Error,
  };
  State m_state = State::Loading;

  CustomAssetCache* m_asset_cache;
  // Note: asset cache owns the asset, we access as a reference
  TextureAsset* m_texture_asset = nullptr;

  std::shared_ptr<CustomTextureData> m_current_texture_data;
  TimePoint m_current_time = {};
};
}  // namespace VideoCommon
