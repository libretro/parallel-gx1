// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/HookableEvent.h"

#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/Resources/TextureDataResource.h"

namespace VideoCommon
{
class CustomResourceManager
{
public:
  void Initialize();
  void Shutdown();
  void Reset();

  void XFBTriggered();

  TextureDataResource* GetTextureDataFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                                               VideoCommon::DirectFilesystemAssetLibrary* library);

private:
  CustomAssetCache m_asset_cache;

  Common::EventHook m_xfb_event;
};
}  // namespace VideoCommon
