// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/Resources/TextureDataResource.h"

#include <cstdlib>
#include <cstring>

#include <retro_miscellaneous.h>

#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
void CustomAssetCache::Initialize()
{
  // Use half of available system memory but leave at least 2GiB unused for system stability.
  constexpr size_t must_keep_unused = 2 * size_t(1024 * 1024 * 1024);

  const size_t sys_mem = Common::MemPhysical();
  const size_t keep_unused_mem = MAX(sys_mem / 2, MIN(sys_mem, must_keep_unused));

  m_max_ram_available = sys_mem - keep_unused_mem;

  if (m_max_ram_available == 0)
    ERROR_LOG(VIDEO, "Not enough system memory for custom resources.");

  m_asset_loader.Initialize();
}

CustomAssetCache::~CustomAssetCache()
{
  // no worker may hold an asset as it goes
  m_asset_loader.Reset(false);
  FreeAssets();
}

void CustomAssetCache::Shutdown()
{
  Reset();

  m_asset_loader.Shutdown();
}

void CustomAssetCache::Reset()
{
  m_asset_loader.Reset(true);
  FreeAssets();
}

void CustomAssetCache::FreeAssets()
{
  for (u32 i = 0; i < m_num_assets; i++)
  {
    delete m_assets[i].listener;
    delete m_assets[i].asset;
  }
  free(m_assets);
  free(m_index);
  free(m_schedule);
  m_assets = nullptr;
  m_index = nullptr;
  m_schedule = nullptr;
  m_num_assets = 0;
  m_assets_capacity = 0;
  m_index_capacity = 0;
  m_queues[ACTIVE] = {};
  m_queues[PENDING] = {};
  m_ram_used = 0;
}

static u32 HashAssetId(const CustomAssetLibrary::AssetID& asset_id)
{
  // FNV-1a
  u32 hash = 2166136261u;
  for (const char c : asset_id)
    hash = (hash ^ static_cast<u8>(c)) * 16777619u;
  return hash;
}

u32 CustomAssetCache::FindHandle(const CustomAssetLibrary::AssetID& asset_id, u32* slot) const
{
  if (!m_index_capacity)
    return NO_ASSET;
  const u32 mask = m_index_capacity - 1;
  for (u32 i = HashAssetId(asset_id) & mask;; i = (i + 1) & mask)
  {
    const u32 handle = m_index[i];
    if (handle == NO_ASSET || m_assets[handle].asset->GetAssetId() == asset_id)
    {
      if (slot)
        *slot = i;
      return handle;
    }
  }
}

void CustomAssetCache::GrowIndex()
{
  const u32 capacity = m_index_capacity ? m_index_capacity * 2 : 64;
  u32* const index = static_cast<u32*>(malloc(capacity * sizeof(u32)));
  if (!index)
    abort();
  memset(index, 0xFF, capacity * sizeof(u32));
  for (u32 handle = 0; handle < m_num_assets; handle++)
  {
    u32 i = HashAssetId(m_assets[handle].asset->GetAssetId()) & (capacity - 1);
    while (index[i] != NO_ASSET)
      i = (i + 1) & (capacity - 1);
    index[i] = handle;
  }
  free(m_index);
  m_index = index;
  m_index_capacity = capacity;
}

TextureDataResource*
CustomAssetCache::FindResource(const CustomAssetLibrary::AssetID& asset_id) const
{
  const u32 handle = FindHandle(asset_id, nullptr);
  return handle == NO_ASSET ? nullptr : m_assets[handle].listener;
}

TextureAsset* CustomAssetCache::CreateAsset(const CustomAssetLibrary::AssetID& asset_id,
                                            VideoCommon::DirectFilesystemAssetLibrary* library,
                                            TextureDataResource* listener)
{
  u32 slot;
  u32 handle = FindHandle(asset_id, &slot);
  if (handle == NO_ASSET)
  {
    // keep the index at most half full
    if ((m_num_assets + 1) * 2 > m_index_capacity)
    {
      GrowIndex();
      FindHandle(asset_id, &slot);
    }
    if (m_num_assets == m_assets_capacity)
    {
      const u32 capacity = m_assets_capacity ? m_assets_capacity * 2 : 64;
      AssetData* const assets =
          static_cast<AssetData*>(realloc(m_assets, capacity * sizeof(AssetData)));
      TextureAsset** const schedule =
          static_cast<TextureAsset**>(realloc(m_schedule, capacity * sizeof(TextureAsset*)));
      if (assets)
        m_assets = assets;
      if (schedule)
        m_schedule = schedule;
      if (!assets || !schedule)
        abort();
      m_assets_capacity = capacity;
    }
    handle = m_num_assets++;
    AssetData& data = m_assets[handle];
    data.asset = new TextureAsset(library, asset_id, handle);
    data.prev[ACTIVE] = data.next[ACTIVE] = data.prev[PENDING] = data.next[PENDING] = NO_ASSET;
    data.queued[ACTIVE] = data.queued[PENDING] = false;
    m_index[slot] = handle;
  }

  m_assets[handle].listener = listener;
  return m_assets[handle].asset;
}

void CustomAssetCache::QueueRemove(u32 queue, u32 handle)
{
  AssetData& data = m_assets[handle];
  if (!data.queued[queue])
    return;
  Queue& q = m_queues[queue];
  if (data.prev[queue] != NO_ASSET)
    m_assets[data.prev[queue]].next[queue] = data.next[queue];
  else
    q.head = data.next[queue];
  if (data.next[queue] != NO_ASSET)
    m_assets[data.next[queue]].prev[queue] = data.prev[queue];
  else
    q.tail = data.prev[queue];
  data.prev[queue] = data.next[queue] = NO_ASSET;
  data.queued[queue] = false;
  q.count--;
}

void CustomAssetCache::QueuePushFront(u32 queue, u32 handle)
{
  QueueRemove(queue, handle);
  AssetData& data = m_assets[handle];
  Queue& q = m_queues[queue];
  data.prev[queue] = NO_ASSET;
  data.next[queue] = q.head;
  if (q.head != NO_ASSET)
    m_assets[q.head].prev[queue] = handle;
  else
    q.tail = handle;
  q.head = handle;
  data.queued[queue] = true;
  q.count++;
}

void CustomAssetCache::QueueInsert(u32 queue, u32 handle)
{
  AssetData& data = m_assets[handle];
  if (data.queued[queue])
    return;
  Queue& q = m_queues[queue];
  data.next[queue] = NO_ASSET;
  data.prev[queue] = q.tail;
  if (q.tail != NO_ASSET)
    m_assets[q.tail].next[queue] = handle;
  else
    q.head = handle;
  q.tail = handle;
  data.queued[queue] = true;
  q.count++;
}

void CustomAssetCache::MarkAssetPending(TextureAsset* asset)
{
  QueuePushFront(PENDING, static_cast<u32>(asset->GetHandle()));
}

void CustomAssetCache::MarkAssetActive(TextureAsset* asset)
{
  QueuePushFront(ACTIVE, static_cast<u32>(asset->GetHandle()));
}

void CustomAssetCache::Update()
{
  ProcessLoadedAssets();

  if (m_ram_used > m_max_ram_available)
  {
    RemoveAssetsUntilBelowMemoryLimit();
  }

  if (!m_queues[PENDING].count)
    return;

  if (m_ram_used > m_max_ram_available)
    return;

  u32 count = 0;
  for (u32 handle = m_queues[PENDING].head; handle != NO_ASSET;
       handle = m_assets[handle].next[PENDING])
    m_schedule[count++] = m_assets[handle].asset;

  const u64 allowed_memory = m_max_ram_available - m_ram_used;
  m_asset_loader.ScheduleAssetsToLoad(m_schedule, count, allowed_memory);
}

void CustomAssetCache::OnAssetLoaded(void* context, TextureAsset* asset, bool loaded)
{
  CustomAssetCache* const cache = static_cast<CustomAssetCache*>(context);
  const u32 handle = static_cast<u32>(asset->GetHandle());
  cache->QueueRemove(PENDING, handle);

  if (loaded)
  {
    cache->QueueInsert(ACTIVE, handle);
    cache->m_assets[handle].listener->NotifyAssetLoadSuccess();
  }
  else
  {
    cache->m_assets[handle].listener->NotifyAssetLoadFailed();
  }
}

void CustomAssetCache::ProcessLoadedAssets()
{
  // Update the ram with the change in memory from the loader
  //
  // Note: Assets with outstanding reload requests will have
  // two copies in memory temporarily (the old data stored in
  // the asset shared_ptr that the resource manager owns, and
  // the new data loaded from the loader in the asset's shared_ptr)
  // This temporary duplication will not be reflected in the
  // resource manager's ram used
  m_ram_used += m_asset_loader.TakeLoadResults(OnAssetLoaded, this);
}

void CustomAssetCache::RemoveAssetsUntilBelowMemoryLimit()
{
  const u64 threshold_ram = m_max_ram_available * 8 / 10;

  if (m_ram_used > threshold_ram)
  {
    INFO_LOG(VIDEO, "Memory usage over threshold: %s", UICommon::FormatSize(m_ram_used).c_str());
  }

  // Clear out least recently used resources until
  // we get safely in our threshold
  while (m_ram_used > threshold_ram && m_queues[ACTIVE].count > 0)
  {
    TextureAsset* const asset = m_assets[m_queues[ACTIVE].tail].asset;
    QueueRemove(ACTIVE, m_queues[ACTIVE].tail);

    // Remove the asset's copy
    const std::size_t bytes_unloaded = asset->Unload();
    m_ram_used -= bytes_unloaded;

    INFO_LOG(VIDEO, "Unloading asset: %s (%s)", asset->GetAssetId().c_str(),
             UICommon::FormatSize(bytes_unloaded).c_str());
  }
}

}  // namespace VideoCommon
