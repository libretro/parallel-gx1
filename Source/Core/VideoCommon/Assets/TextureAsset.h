// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Common/Quiesce.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/CustomTextureData.h"

namespace VideoCommon
{
// A custom texture loaded from a 'DirectFilesystemAssetLibrary', its data published for
// readers on any thread
class TextureAsset
{
public:
  TextureAsset(std::shared_ptr<DirectFilesystemAssetLibrary> library,
               CustomAssetLibrary::AssetID asset_id, u64 session_id);
  ~TextureAsset();
  TextureAsset(const TextureAsset&) = delete;
  TextureAsset(TextureAsset&&) = delete;
  TextureAsset& operator=(const TextureAsset&) = delete;
  TextureAsset& operator=(TextureAsset&&) = delete;

  // Loads the asset from the library returning the number of bytes loaded. Returns 0 without
  // doing anything when another thread is loading or unloading this asset right now.
  std::size_t Load();

  // Unloads the asset data, resets the bytes loaded and
  // returns the number of bytes unloaded; 0 if another thread is busy with it.
  std::size_t Unload();

  // Callees should understand that the type returned is
  // a local copy and 'GetData()' needs to be called
  // to ensure the latest copy is available if
  // they want to handle reloads
  [[nodiscard]] std::shared_ptr<CustomTextureData> GetData() const;

  // Returns the time that the data was last loaded
  TimePoint GetLastLoadedTime() const;

  // Returns an id that uniquely identifies this asset
  const CustomAssetLibrary::AssetID& GetAssetId() const { return m_asset_id; }

  // Returns an id that is unique to this game session
  // This is a faster form to hash and can be used
  // as an index
  std::size_t GetHandle() const { return m_handle; }

  // The loader's report that a load finished, carried by the asset itself. Worker: records
  // whether it loaded; true when the asset is to be pushed through LoadResultLink, false when
  // it is still waiting to be taken. Taker: TakeLoadResult gives back the latest outcome.
  bool PostLoadResult(bool loaded);
  mpsc_stack_node_t* LoadResultLink() { return &m_load_result.link; }
  static TextureAsset* FromLoadResultLink(mpsc_stack_node_t* node)
  {
    return reinterpret_cast<LoadResult*>(node)->asset;
  }
  bool TakeLoadResult();

private:
  struct LoadResult
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    TextureAsset* asset;
    retro_atomic_int_t queued;
    retro_atomic_int_t loaded;
  };

  struct Holder
  {
    quiesce_node_t node;  // first: the free callback casts it back
    std::shared_ptr<CustomTextureData> data;
  };

  static void FreeHolder(quiesce_node_t* node);

  // Publishes freshly loaded data; null unloads.
  void SetData(std::shared_ptr<CustomTextureData> data);

  const std::shared_ptr<DirectFilesystemAssetLibrary> m_owning_library;
  CustomAssetLibrary::AssetID m_asset_id;
  std::size_t m_handle;

  // Held (with a CAS) by the one thread loading or unloading; anyone else backs off.
  retro_atomic_int_t m_busy{};
  std::size_t m_bytes_loaded = 0;         // only touched while busy
  retro_atomic_64_t m_last_loaded_time{}; /* TimePoint ticks */

  mutable retro_atomic_ptr_t m_data{};

  LoadResult m_load_result{};
};
}  // namespace VideoCommon
