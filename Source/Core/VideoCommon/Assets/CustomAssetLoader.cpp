// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/CustomAssetLoader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "UICommon/UICommon.h"

namespace VideoCommon
{
CustomAssetLoader::~CustomAssetLoader()
{
  Reset(false);
  for (mpsc_stack_node_t* node = mpsc_stack_drain(&m_spare_batches); node;)
  {
    Batch* const batch = reinterpret_cast<Batch*>(node);
    node = node->next;
    free(batch->assets);
    delete batch;
  }
  retro_eventcount_free(&m_worker_thread_wake);
}

void CustomAssetLoader::Initialize()
{
  ResizeWorkerThreads(2);
}

void CustomAssetLoader::Shutdown()
{
  Reset(false);
}

bool CustomAssetLoader::StartWorkerThreads(u32 num_worker_threads)
{
  if (num_worker_threads > MAX_WORKER_THREADS)
    num_worker_threads = MAX_WORKER_THREADS;
  for (u32 i = 0; i < num_worker_threads; i++)
  {
    WorkerThread& worker = m_worker_threads[m_num_worker_threads];
    worker = WorkerThread{this, i, nullptr};
    worker.thread = sthread_create(WorkerThreadEntry, &worker);
    if (!worker.thread)
      break;
    m_num_worker_threads++;
  }

  return HasWorkerThreads();
}

bool CustomAssetLoader::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_num_worker_threads == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

bool CustomAssetLoader::HasWorkerThreads() const
{
  return m_num_worker_threads != 0;
}

void CustomAssetLoader::StopWorkerThreads()
{
  if (!HasWorkerThreads())
    return;

  // Signal worker threads to stop, and wake all of them.
  m_exit_flag.Set();
  retro_eventcount_notify(&m_worker_thread_wake);

  // Wait for worker threads to exit.
  for (u32 i = 0; i < m_num_worker_threads; i++)
    Common::JoinThread(m_worker_threads[i].thread);
  m_num_worker_threads = 0;
  m_exit_flag.Clear();
}

void CustomAssetLoader::WorkerThreadEntry(void* data)
{
  const WorkerThread* const worker = static_cast<const WorkerThread*>(data);
  worker->owner->WorkerThreadRun(worker->index);
}

void CustomAssetLoader::RetireBatch(quiesce_node_t* node)
{
  Batch* const batch = reinterpret_cast<Batch*>(node);
  mpsc_stack_push(&batch->owner->m_spare_batches, &batch->node.link);
}

void CustomAssetLoader::WorkerThreadRun(u32 thread_index)
{
#if defined(SKIP_ASSETLOADER_THREAD)
  return;
#endif
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "Asset Loader %d", static_cast<int>(thread_index));
  Common::SetCurrentThreadName(thread_name);

  while (!m_exit_flag.IsSet())
  {
    // Claim the next asset of the current batch, if it has one left.
    TextureAsset* item = nullptr;
    quiesce_enter(&m_batch_quiesce);
    if (auto* batch = static_cast<Batch*>(retro_atomic_load_acquire_ptr(&m_batch)))
    {
      // If more memory than allowed has already been loaded, we will load nothing more
      //  until the next ScheduleAssetsToLoad from Manager.
      if (retro_atomic_load_acquire_64(&m_change_in_memory) > batch->allowed_memory)
        retro_atomic_store_release_size(&batch->cursor, batch->count);

      const size_t index = retro_atomic_fetch_add_size(&batch->cursor, 1);
      if (index < batch->count)
        item = batch->assets[index];
    }
    quiesce_exit(&m_batch_quiesce);

    if (!item)
    {
      // Nothing left: park until a new batch or the exit flag. The check follows the
      // registration, so a notify in between is not lost.
      const int key = retro_eventcount_prepare_wait(&m_worker_thread_wake);
      bool has_work = m_exit_flag.IsSet();
      quiesce_enter(&m_batch_quiesce);
      if (const auto* batch = static_cast<const Batch*>(retro_atomic_load_acquire_ptr(&m_batch)))
        has_work |= retro_atomic_load_acquire_size(
                        const_cast<retro_atomic_size_t*>(&batch->cursor)) < batch->count;
      quiesce_exit(&m_batch_quiesce);
      if (has_work)
        retro_eventcount_cancel_wait(&m_worker_thread_wake);
      else
        retro_eventcount_commit_wait(&m_worker_thread_wake, key);
      continue;
    }

    // Unload previously loaded asset. An asset another thread is busy with comes back 0
    // and is skipped; the Manager schedules it again.
    retro_atomic_fetch_sub_64(&m_change_in_memory, static_cast<s64>(item->Unload()));

    const std::size_t bytes_loaded = item->Load();
    retro_atomic_fetch_add_64(&m_change_in_memory, static_cast<s64>(bytes_loaded));

    INFO_LOG(VIDEO, "CustomAssetLoader thread %u loaded: %s (%s)", thread_index,
             item->GetAssetId().c_str(), UICommon::FormatSize(bytes_loaded).c_str());

    if (item->PostLoadResult(bytes_loaded > 0))
      mpsc_stack_push(&m_loaded, item->LoadResultLink());
  }
}

s64 CustomAssetLoader::TakeLoadResults(void (*on_loaded)(void* context, TextureAsset* asset,
                                                         bool loaded),
                                       void* context)
{
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_loaded)); node;)
  {
    TextureAsset* const asset = TextureAsset::FromLoadResultLink(node);
    node = node->next;
    const bool loaded = asset->TakeLoadResult();
    if (on_loaded)
      on_loaded(context, asset, loaded);
  }
  return retro_atomic_exchange_64(&m_change_in_memory, 0);
}

void CustomAssetLoader::ScheduleAssetsToLoad(TextureAsset* const* assets, u32 count,
                                             u64 allowed_memory)
{
  if (!count) [[unlikely]]
    return;

  // A spare batch, its array grown when the assets outnumber it; a new one only at first.
  Batch* batch = reinterpret_cast<Batch*>(mpsc_stack_drain(&m_spare_batches));
  if (batch)
  {
    // the rest go back for later schedules
    for (mpsc_stack_node_t* rest = batch->node.link.next; rest;)
    {
      mpsc_stack_node_t* const next = rest->next;
      mpsc_stack_push(&m_spare_batches, rest);
      rest = next;
    }
  }
  else
  {
    batch = new Batch{{}, this, nullptr, 0, 0, 0, {}};
  }
  if (batch->capacity < count)
  {
    u32 capacity = batch->capacity ? batch->capacity : 64;
    while (capacity < count)
      capacity *= 2;
    TextureAsset** const array =
        static_cast<TextureAsset**>(realloc(batch->assets, capacity * sizeof(TextureAsset*)));
    if (!array)
      abort();
    batch->assets = array;
    batch->capacity = capacity;
  }
  memcpy(batch->assets, assets, count * sizeof(TextureAsset*));
  batch->count = count;
  batch->allowed_memory = static_cast<s64>(allowed_memory);
  retro_atomic_size_init(&batch->cursor, 0);

  // There's new assets to process, publish them and notify worker threads.
  if (void* old = retro_atomic_exchange_ptr(&m_batch, batch))
    quiesce_retire(&m_batch_quiesce, &static_cast<Batch*>(old)->node, RetireBatch);
  retro_eventcount_notify(&m_worker_thread_wake);
}

void CustomAssetLoader::Reset(bool restart_worker_threads)
{
  const u32 worker_thread_count = m_num_worker_threads;
  StopWorkerThreads();

  // No worker runs now.
  if (auto* batch = static_cast<Batch*>(retro_atomic_exchange_ptr(&m_batch, nullptr)))
    mpsc_stack_push(&m_spare_batches, &batch->node.link);
  quiesce_deinit(&m_batch_quiesce);
  TakeLoadResults(nullptr, nullptr);
  retro_atomic_store_release_64(&m_change_in_memory, 0);

  if (restart_worker_threads)
  {
    StartWorkerThreads(worker_thread_count);
  }
}

}  // namespace VideoCommon
