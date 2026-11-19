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
  m_texture_data_resources.clear();

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
  auto& resource = m_texture_data_resources[asset_id];
  if (resource == nullptr)
  {
    resource = std::make_unique<TextureDataResource>(asset_id, std::move(library), &m_asset_cache);
  }
  resource->Process();
  return resource.get();
}

}  // namespace VideoCommon
