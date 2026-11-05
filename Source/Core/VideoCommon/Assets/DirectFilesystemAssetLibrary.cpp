// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"

#include <vector>

#include <fmt/std.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/System.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/Assets/TextureAssetUtils.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/Resources/CustomResourceManager.h"

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

struct AssetIDValue
{
  quiesce_node_t node;
  CustomAssetLibrary::AssetID asset_id;
};

void FreeAssetMapValue(quiesce_node_t* node)
{
  delete reinterpret_cast<AssetMapValue*>(node);
}

void FreeAssetIDValue(quiesce_node_t* node)
{
  delete reinterpret_cast<AssetIDValue*>(node);
}
}  // namespace

DirectFilesystemAssetLibrary::DirectFilesystemAssetLibrary()
    : m_asset_id_to_asset_map_path(atomic_value_map_new(4096, FreeAssetMapValue)),
      m_path_to_asset_id(atomic_value_map_new(4096, FreeAssetIDValue))
{
}

DirectFilesystemAssetLibrary::~DirectFilesystemAssetLibrary()
{
  atomic_value_map_free(m_path_to_asset_id);
  atomic_value_map_free(m_asset_id_to_asset_map_path);
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadTexture(const AssetID& asset_id,
                                                                       CustomTextureData* data)
{
  const auto asset_map = GetAssetMapForID(asset_id);
  if (asset_map.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - raw texture expected to have one or two files mapped!",
                  asset_id);
    return {};
  }

  const auto texture_path = asset_map.find("texture");

  if (texture_path == asset_map.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a texture entry mapped!", asset_id);
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
  AssetMapValue* value;

  atomic_value_map_enter(m_asset_id_to_asset_map_path);
  value = reinterpret_cast<AssetMapValue*>(
      atomic_value_map_get(m_asset_id_to_asset_map_path, asset_id.data(), asset_id.size()));
  if (value)
  {
    for (const auto& [name, path] : value->map)
    {
      const std::string key = PathToString(path);
      atomic_value_map_set(m_path_to_asset_id, key.data(), key.size(), nullptr);
    }
  }
  atomic_value_map_exit(m_asset_id_to_asset_map_path);

  for (const auto& [name, path] : asset_path_map)
  {
    const std::string key = PathToString(path);
    AssetIDValue* id_value = new AssetIDValue{{}, asset_id};
    atomic_value_map_set(m_path_to_asset_id, key.data(), key.size(), &id_value->node);
  }

  value = new AssetMapValue{{}, std::move(asset_path_map)};
  atomic_value_map_set(m_asset_id_to_asset_map_path, asset_id.data(), asset_id.size(),
                       &value->node);
}

void DirectFilesystemAssetLibrary::PathModified(std::string_view path)
{
  AssetID asset_id;
  bool found = false;

  atomic_value_map_enter(m_path_to_asset_id);
  const AssetIDValue* value = reinterpret_cast<const AssetIDValue*>(
      atomic_value_map_get(m_path_to_asset_id, path.data(), path.size()));
  if (value)
  {
    asset_id = value->asset_id;
    found = true;
  }
  atomic_value_map_exit(m_path_to_asset_id);

  if (found)
  {
    auto& system = Core::System::GetInstance();
    auto& resource_manager = system.GetCustomResourceManager();
    resource_manager.MarkAssetDirty(asset_id);
  }
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
