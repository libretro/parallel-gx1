// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/TextureAsset.h"

#include <utility>

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"

namespace VideoCommon
{
// Asset data is freed through this once no thread is reading it.
static quiesce_t s_data_quiesce;

void TextureAsset::FreeHolder(quiesce_node_t* node)
{
  delete reinterpret_cast<Holder*>(node);
}

TextureAsset::TextureAsset(std::shared_ptr<DirectFilesystemAssetLibrary> library,
                           CustomAssetLibrary::AssetID asset_id, u64 asset_handle)
    : m_owning_library(std::move(library)), m_asset_id(std::move(asset_id)), m_handle(asset_handle)
{
  m_load_result.asset = this;
}

bool TextureAsset::PostLoadResult(bool loaded)
{
  retro_atomic_store_release_int(&m_load_result.loaded, loaded ? 1 : 0);
  return !retro_atomic_exchange_int(&m_load_result.queued, 1);
}

bool TextureAsset::TakeLoadResult()
{
  // cleared first: a load that ends after this is posted again
  retro_atomic_exchange_int(&m_load_result.queued, 0);
  return retro_atomic_load_acquire_int(&m_load_result.loaded) != 0;
}

TextureAsset::~TextureAsset()
{
  delete static_cast<Holder*>(retro_atomic_exchange_ptr(&m_data, nullptr));
}

std::shared_ptr<CustomTextureData> TextureAsset::GetData() const
{
  quiesce_enter(&s_data_quiesce);
  const auto* holder = static_cast<const Holder*>(retro_atomic_load_acquire_ptr(&m_data));
  std::shared_ptr<CustomTextureData> data = holder ? holder->data : nullptr;
  quiesce_exit(&s_data_quiesce);
  return data;
}

void TextureAsset::SetData(std::shared_ptr<CustomTextureData> data)
{
  Holder* const next = data ? new Holder{{}, std::move(data)} : nullptr;
  if (void* old = retro_atomic_exchange_ptr(&m_data, next))
    quiesce_retire(&s_data_quiesce, &static_cast<Holder*>(old)->node, FreeHolder);
}

std::size_t TextureAsset::Load()
{
  if (!retro_atomic_cas_int(&m_busy, 0, 1))
    return 0;
  // The load time needs to come from before the data is actually read.
  // Using a time point from after the read marks the asset as more up-to-date than it actually is,
  //  and has potential to race (and not be updated) if a change happens immediately after load.
  const auto load_time = Clock::now();

  std::size_t bytes_loaded = 0;
  auto potential_data = std::make_shared<CustomTextureData>();
  const auto loaded_info = m_owning_library->LoadTexture(m_asset_id, potential_data.get());
  if (loaded_info.bytes_loaded > 0)
  {
    SetData(std::move(potential_data));
    bytes_loaded = loaded_info.bytes_loaded;
    m_bytes_loaded = bytes_loaded;
    retro_atomic_store_release_64(&m_last_loaded_time, load_time);
  }
  retro_atomic_store_release_int(&m_busy, 0);
  return bytes_loaded;
}

std::size_t TextureAsset::Unload()
{
  if (!retro_atomic_cas_int(&m_busy, 0, 1))
    return 0;
  SetData(nullptr);
  const std::size_t unloaded = std::exchange(m_bytes_loaded, 0);
  retro_atomic_store_release_int(&m_busy, 0);
  return unloaded;
}

TimePoint TextureAsset::GetLastLoadedTime() const
{
  return retro_atomic_load_acquire_64(&m_last_loaded_time);
}
}  // namespace VideoCommon
