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
  m_worker_thread.Reset("resource-worker");

  m_xfb_event =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { XFBTriggered(); });
}

void CustomResourceManager::Shutdown()
{
  m_asset_cache.Shutdown();
  m_worker_thread.Shutdown();
  Reset(true);
}

void CustomResourceManager::Reset(bool is_shutdown)
{
  m_texture_data_resources.clear();

  m_asset_cache.Reset();

  if (is_shutdown)
    m_worker_thread.Shutdown();
  else
    m_worker_thread.Reset("resource-worker");
}

void CustomResourceManager::MarkAssetDirty(const CustomAssetLibrary::AssetID& asset_id)
{
  m_asset_cache.MarkAssetDirty(asset_id);
}

void CustomResourceManager::XFBTriggered()
{
  m_asset_cache.Update();
}

TextureDataResource* CustomResourceManager::GetTextureDataFromAsset(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  auto& resource = m_texture_data_resources[asset_id];
  if (resource == nullptr)
  {
    resource =
        std::make_unique<TextureDataResource>(CreateResourceContext(asset_id, std::move(library)));
  }
  resource->Process();
  return resource.get();
}

Resource::ResourceContext CustomResourceManager::CreateResourceContext(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return Resource::ResourceContext{asset_id, std::move(library), &m_asset_cache, this,
                                   &m_worker_thread};
}
}  // namespace VideoCommon
