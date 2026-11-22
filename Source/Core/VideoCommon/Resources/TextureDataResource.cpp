// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Resources/TextureDataResource.h"

#include "VideoCommon/Assets/CustomAssetCache.h"

namespace VideoCommon
{
TextureDataResource::TextureDataResource(const CustomAssetLibrary::AssetID& asset_id,
                                         DirectFilesystemAssetLibrary* library,
                                         CustomAssetCache* asset_cache)
    : m_asset_cache(asset_cache)
{
  m_texture_asset = m_asset_cache->CreateAsset(asset_id, library, this);
}

TextureDataResource::~TextureDataResource()
{
  if (m_current_texture_data)
    m_current_texture_data->Release();
}

void TextureDataResource::Process()
{
  m_asset_cache->MarkAssetActive(m_texture_asset);

  if (m_state != State::Loading)
    return;

  const auto last_load_time = m_texture_asset->GetLastLoadedTime();
  CustomTextureData* const data = m_texture_asset->GetData();
  if (!data)
  {
    m_asset_cache->MarkAssetPending(m_texture_asset);
    return;
  }

  if (m_current_texture_data)
    m_current_texture_data->Release();
  m_current_texture_data = data;
  m_current_time = last_load_time;
  m_state = State::Ready;
}
}  // namespace VideoCommon
