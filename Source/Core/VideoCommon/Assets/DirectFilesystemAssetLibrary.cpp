// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"

#include <vector>

#include <cstdio>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/Assets/TextureAssetUtils.h"
#include "VideoCommon/RenderState.h"

namespace VideoCommon
{
namespace
{
std::size_t GetAssetSize(const CustomTextureData& data)
{
  std::size_t total = 0;
  for (const auto& slice : data.m_slices)
  {
    for (const auto& level : slice.m_levels)
    {
      total += level.data.size();
    }
  }
  return total;
}

// The maps' values: each begins with the node the map frees it through.
struct AssetMapValue
{
  quiesce_node_t node;
  Assets::AssetMap map;
};

void FreeAssetMapValue(quiesce_node_t* node)
{
  delete reinterpret_cast<AssetMapValue*>(node);
}
}  // namespace

DirectFilesystemAssetLibrary::DirectFilesystemAssetLibrary()
    : m_asset_id_to_asset_map_path(atomic_value_map_new(4096, FreeAssetMapValue))
{
}

DirectFilesystemAssetLibrary::~DirectFilesystemAssetLibrary()
{
  atomic_value_map_free(m_asset_id_to_asset_map_path);
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadTexture(const AssetID& asset_id,
                                                                       CustomTextureData* data)
{
  const auto asset_map = GetAssetMapForID(asset_id);
  if (asset_map.empty())
  {
    ERROR_LOG(VIDEO, "Asset '%s' error - raw texture expected to have one or two files mapped!",
              asset_id.c_str());
    return {};
  }

  const auto texture_path = asset_map.find("texture");

  if (texture_path == asset_map.end())
  {
    ERROR_LOG(VIDEO, "Asset '%s' expected to have a texture entry mapped!", asset_id.c_str());
    return {};
  }

  if (!LoadTextureDataFromFile(asset_id, texture_path->second, AbstractTextureType::Texture_2D,
                               data))
  {
    return {};
  }
  if (!PurgeInvalidMipsFromTextureData(asset_id, data))
    return {};

  return LoadInfo{GetAssetSize(*data)};
}

void DirectFilesystemAssetLibrary::SetAssetIDMapData(const AssetID& asset_id,
                                                     VideoCommon::Assets::AssetMap asset_path_map)
{
  AssetMapValue* value = new AssetMapValue{{}, std::move(asset_path_map)};
  atomic_value_map_set(m_asset_id_to_asset_map_path, asset_id.data(), asset_id.size(),
                       &value->node);
}

VideoCommon::Assets::AssetMap
DirectFilesystemAssetLibrary::GetAssetMapForID(const AssetID& asset_id) const
{
  VideoCommon::Assets::AssetMap result;

  atomic_value_map_enter(m_asset_id_to_asset_map_path);
  const AssetMapValue* value = reinterpret_cast<const AssetMapValue*>(
      atomic_value_map_get(m_asset_id_to_asset_map_path, asset_id.data(), asset_id.size()));
  if (value)
    result = value->map;
  atomic_value_map_exit(m_asset_id_to_asset_map_path);
  return result;
}
}  // namespace VideoCommon
