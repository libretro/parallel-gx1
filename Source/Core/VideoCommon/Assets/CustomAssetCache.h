// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/CustomAssetLoader.h"
#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
class TextureDataResource;

// The asset cache manages custom assets (textures, shaders, meshes).
// These assets are loaded using a priority system,
// where assets requested more often gets loaded first.  This system
// also tracks memory usage and if memory usage goes over a calculated limit,
// then assets will be purged with older assets being targeted first.
class CustomAssetCache
{
public:
  CustomAssetCache() = default;
  ~CustomAssetCache();
  CustomAssetCache(const CustomAssetCache&) = delete;
  CustomAssetCache& operator=(const CustomAssetCache&) = delete;

  // The asset asset_id names, made on its first request; listener hears how its loads end.
  TextureAsset* CreateAsset(const CustomAssetLibrary::AssetID& asset_id,
                            std::shared_ptr<VideoCommon::DirectFilesystemAssetLibrary> library,
                            TextureDataResource* listener);

  void Initialize();
  void Shutdown();

  void Reset();

  // Notify the system that we are interested in this asset and
  // are waiting for it to be loaded
  void MarkAssetPending(TextureAsset* asset);

  // Notify the system we are interested in this asset and
  // it has seen activity
  void MarkAssetActive(TextureAsset* asset);

  void Update();

private:
  enum : u32
  {
    NO_ASSET = 0xFFFFFFFFu,
    // the two queues every asset can be in, each linked through the assets themselves
    ACTIVE = 0,
    PENDING = 1,
  };

  // One per asset, indexed by its handle (handles count up from 0 as assets are made).
  struct AssetData
  {
    TextureAsset* asset;  // owned
    TextureDataResource* listener;
    u32 prev[2];  // per queue: the asset in front, NO_ASSET at the head
    u32 next[2];  // per queue: the asset behind, NO_ASSET at the tail
    bool queued[2];
  };

  // Assets in order of most recent use, head first; used to pick which to load or unload.
  struct Queue
  {
    u32 head = NO_ASSET;
    u32 tail = NO_ASSET;
    u32 count = 0;
  };

  void QueueRemove(u32 queue, u32 handle);
  // Inserts or moves the asset to the head.
  void QueuePushFront(u32 queue, u32 handle);
  // Inserts the asset at the tail, or leaves it where it is when it is already in.
  void QueueInsert(u32 queue, u32 handle);

  // The handle of asset_id, NO_ASSET when it has none yet; slot gets where it would go.
  u32 FindHandle(const CustomAssetLibrary::AssetID& asset_id, u32* slot) const;
  void GrowIndex();

  void FreeAssets();
  static void OnAssetLoaded(void* context, TextureAsset* asset, bool loaded);
  void ProcessLoadedAssets();
  void RemoveAssetsUntilBelowMemoryLimit();

  AssetData* m_assets = nullptr;
  u32 m_num_assets = 0;
  u32 m_assets_capacity = 0;

  // Open-addressed: asset id hash to handle, NO_ASSET where free; a power of two in size.
  u32* m_index = nullptr;
  u32 m_index_capacity = 0;

  // Assets that are currently active in memory, in order of most recently used by the game.
  Queue m_queues[2];
  // The pending queue as the loader takes it, kept between frames.
  TextureAsset** m_schedule = nullptr;

  // Memory used by currently "loaded" assets.
  u64 m_ram_used = 0;

  // A calculated amount of memory to avoid exceeding.
  u64 m_max_ram_available = 0;

  CustomAssetLoader m_asset_loader;
};

}  // namespace VideoCommon
