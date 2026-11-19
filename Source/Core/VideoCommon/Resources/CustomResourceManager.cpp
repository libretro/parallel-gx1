// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Resources/CustomResourceManager.h"

#include "VideoCommon/VideoEvents.h"

namespace VideoCommon
{
void CustomResourceManager::Initialize()
{
#if defined(SKIP_RESOURCE_WORKER_THREAD)
  return;
#endif
  m_asset_cache.Initialize();

  m_xfb_event =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { XFBTriggered(); });
}

void CustomResourceManager::Shutdown()
{
  m_asset_cache.Shutdown();
  Reset();
}

void CustomResourceManager::Reset()
{
  m_asset_cache.Reset();
}

void CustomResourceManager::XFBTriggered()
{
  m_asset_cache.Update();
}

TextureDataResource* CustomResourceManager::GetTextureDataFromAsset(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::DirectFilesystemAssetLibrary> library)
{
  TextureDataResource* resource = m_asset_cache.FindResource(asset_id);
  if (resource == nullptr)
  {
    // the cache keeps it, with its asset
    resource = new TextureDataResource(asset_id, std::move(library), &m_asset_cache);
  }
  resource->Process();
  return resource;
}

}  // namespace VideoCommon
