// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <retro_atomic.h>

#include "Common/Quiesce.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

#include <memory>

namespace VideoCommon
{
// An abstract class that provides operations for loading
// data from a 'CustomAssetLibrary'
class CustomAsset
{
public:
  using ClockType = Clock;
  using TimeType = ClockType::time_point;

  CustomAsset(std::shared_ptr<CustomAssetLibrary> library, CustomAssetLibrary::AssetID asset_id,
              u64 session_id);
  virtual ~CustomAsset() = default;
  CustomAsset(const CustomAsset&) = delete;
  CustomAsset(CustomAsset&&) = delete;
  CustomAsset& operator=(const CustomAsset&) = delete;
  CustomAsset& operator=(CustomAsset&&) = delete;

  // Loads the asset from the library returning the number of bytes loaded. Returns 0 without
  // doing anything when another thread is loading or unloading this asset right now.
  std::size_t Load();

  // Unloads the asset data, resets the bytes loaded and
  // returns the number of bytes unloaded; 0 if another thread is busy with it.
  std::size_t Unload();

  // Asset data is freed through this once no thread is reading it.
  static quiesce_t* DataQuiesce();

  // Returns the time that the data was last loaded
  TimeType GetLastLoadedTime() const;

  // Returns an id that uniquely identifies this asset
  const CustomAssetLibrary::AssetID& GetAssetId() const;

  // Returns an id that is unique to this game session
  // This is a faster form to hash and can be used
  // as an index
  std::size_t GetHandle() const;

protected:
  const std::shared_ptr<CustomAssetLibrary> m_owning_library;

private:
  virtual CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) = 0;
  virtual void UnloadImpl() = 0;
  CustomAssetLibrary::AssetID m_asset_id;
  std::size_t m_handle;

  // Held (with a CAS) by the one thread loading or unloading; anyone else backs off.
  retro_atomic_int_t m_busy{};
  std::size_t m_bytes_loaded = 0;  // only touched while busy
  retro_atomic_64_t m_last_loaded_time{}; /* TimeType ticks */
};

// An abstract class that is expected to
// be the base class for all assets
// It provides a simple interface for
// verifying that an asset data of type
// 'UnderlyingType' is loaded by
// checking against 'GetData()'
template <typename UnderlyingType>
class CustomLoadableAsset : public CustomAsset
{
public:
  using CustomAsset::CustomAsset;

  // Callees should understand that the type returned is
  // a local copy and 'GetData()' needs to be called
  // to ensure the latest copy is available if
  // they want to handle reloads
  [[nodiscard]] std::shared_ptr<UnderlyingType> GetData() const
  {
    quiesce_enter(DataQuiesce());
    const auto* holder = static_cast<const Holder*>(retro_atomic_load_acquire_ptr(&m_data));
    std::shared_ptr<UnderlyingType> data = holder ? holder->data : nullptr;
    quiesce_exit(DataQuiesce());
    return data;
  }

  ~CustomLoadableAsset() override { delete static_cast<Holder*>(retro_atomic_exchange_ptr(&m_data, nullptr)); }

protected:
  // Publishes freshly loaded data; null unloads.
  void SetData(std::shared_ptr<UnderlyingType> data)
  {
    Holder* const next = data ? new Holder{{}, std::move(data)} : nullptr;
    if (void* old = retro_atomic_exchange_ptr(&m_data, next))
      quiesce_retire(DataQuiesce(), &static_cast<Holder*>(old)->node, FreeHolder);
  }

private:
  struct Holder
  {
    quiesce_node_t node;  // first: the free callback casts it back
    std::shared_ptr<UnderlyingType> data;
  };
  static void FreeHolder(quiesce_node_t* node) { delete reinterpret_cast<Holder*>(node); }

  void UnloadImpl() override { SetData(nullptr); }

  mutable retro_atomic_ptr_t m_data{};
};

// A helper struct that contains
// an asset with a last cached write time
// This can be used to determine if the asset
// has diverged from the last known state
// To know if it is time to update other dependencies
// based on the asset data
// TODO C++20: use a 'derived_from' concept against 'CustomAsset' when available
template <typename AssetType>
struct CachedAsset
{
  std::shared_ptr<AssetType> m_asset;
  CustomAsset::TimeType m_cached_write_time;
};

}  // namespace VideoCommon
