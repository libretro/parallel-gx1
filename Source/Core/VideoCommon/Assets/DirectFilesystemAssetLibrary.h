// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <map>
#include <string>

#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/Assets/Types.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

#include "Common/AtomicMap.h"

namespace VideoCommon
{
// Loads any assets directly from the filesystem
class DirectFilesystemAssetLibrary final : public CustomAssetLibrary
{
public:
  DirectFilesystemAssetLibrary();
  ~DirectFilesystemAssetLibrary();
  DirectFilesystemAssetLibrary(const DirectFilesystemAssetLibrary&) = delete;
  DirectFilesystemAssetLibrary& operator=(const DirectFilesystemAssetLibrary&) = delete;

  // Shared by the assets made from it: each keeps a reference, the last deletes it
  void AddRef() { retro_atomic_fetch_add_int(&m_refs, 1); }
  void Release()
  {
    if (retro_atomic_fetch_sub_int(&m_refs, 1) == 1)
      delete this;
  }

  // Loads a texture, if there are no levels, bytes loaded will be empty
  LoadInfo LoadTexture(const AssetID& asset_id, CustomTextureData* data);

  // Assigns the asset id to a map of files, how this map is read is dependent on the data
  // For instance, a raw texture would expect the map to have a single entry and load that
  // file as the asset.  But a model file data might have its data spread across multiple files
  void SetAssetIDMapData(const AssetID& asset_id, Assets::AssetMap asset_path_map);

private:
  // Gets the asset map given an asset id
  Assets::AssetMap GetAssetMapForID(const AssetID& asset_id) const;

  // Written while mods and texture packs load, read by the loader threads: a lock-free map
  // keyed by the string's bytes, each value swapped whole.
  atomic_value_map_t* m_asset_id_to_asset_map_path;  // AssetMapValue

  retro_atomic_int_t m_refs{1};
};
}  // namespace VideoCommon
