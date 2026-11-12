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
// This class implements 'CustomAssetLibrary' and loads any assets
// directly from the filesystem
class DirectFilesystemAssetLibrary final : public CustomAssetLibrary
{
public:
  DirectFilesystemAssetLibrary();
  ~DirectFilesystemAssetLibrary() override;
  DirectFilesystemAssetLibrary(const DirectFilesystemAssetLibrary&) = delete;
  DirectFilesystemAssetLibrary& operator=(const DirectFilesystemAssetLibrary&) = delete;

  LoadInfo LoadTexture(const AssetID& asset_id, CustomTextureData* data) override;

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
};
}  // namespace VideoCommon
