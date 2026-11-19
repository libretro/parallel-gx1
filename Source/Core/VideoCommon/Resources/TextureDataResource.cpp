// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Resources/TextureDataResource.h"

#include "VideoCommon/Assets/CustomAssetCache.h"

namespace VideoCommon
{
TextureDataResource::TextureDataResource(const CustomAssetLibrary::AssetID& asset_id,
                                         std::shared_ptr<DirectFilesystemAssetLibrary> library,
                                         CustomAssetCache* asset_cache)
    : m_asset_cache(asset_cache)
{
  m_texture_asset = m_asset_cache->CreateAsset<TextureAsset>(asset_id, std::move(library), this);
}

void TextureDataResource::Process()
{
  m_asset_cache->MarkAssetActive(m_texture_asset);

  if (m_state != State::Loading)
    return;

  const auto last_load_time = m_texture_asset->GetLastLoadedTime();
  const auto asset = m_texture_asset->GetData();
  if (!asset)
  {
    m_asset_cache->MarkAssetPending(m_texture_asset);
    return;
  }

  m_current_texture_data = asset;
  m_current_time = last_load_time;
  m_state = State::Ready;
}
}  // namespace VideoCommon
