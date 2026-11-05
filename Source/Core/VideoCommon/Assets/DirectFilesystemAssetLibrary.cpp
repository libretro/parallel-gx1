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
}  // namespace

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
  VideoCommon::Assets::AssetMap previous_asset_map;
  {
    std::lock_guard lk(m_asset_map_lock);
    previous_asset_map = m_asset_id_to_asset_map_path[asset_id];
  }

  {
    std::lock_guard lk(m_path_map_lock);
    for (const auto& [name, path] : previous_asset_map)
    {
      m_path_to_asset_id.erase(PathToString(path));
    }

    for (const auto& [name, path] : asset_path_map)
    {
      m_path_to_asset_id[PathToString(path)] = asset_id;
    }
  }

  {
    std::lock_guard lk(m_asset_map_lock);
    m_asset_id_to_asset_map_path[asset_id] = std::move(asset_path_map);
  }
}

void DirectFilesystemAssetLibrary::PathModified(std::string_view path)
{
  std::lock_guard lk(m_path_map_lock);
  if (const auto iter = m_path_to_asset_id.find(path); iter != m_path_to_asset_id.end())
  {
    auto& system = Core::System::GetInstance();
    auto& resource_manager = system.GetCustomResourceManager();
    resource_manager.MarkAssetDirty(iter->second);
  }
}

VideoCommon::Assets::AssetMap
DirectFilesystemAssetLibrary::GetAssetMapForID(const AssetID& asset_id) const
{
  std::lock_guard lk(m_asset_map_lock);
  if (auto iter = m_asset_id_to_asset_map_path.find(asset_id);
      iter != m_asset_id_to_asset_map_path.end())
  {
    return iter->second;
  }
  return {};
}
}  // namespace VideoCommon
