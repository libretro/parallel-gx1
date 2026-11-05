// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Assets/CustomAssetLoader.h"

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "UICommon/UICommon.h"

namespace VideoCommon
{
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
  // Each thread is handed its slot, so the vector must not move once threads are running.
  m_worker_threads.reserve(num_worker_threads);
  for (u32 i = 0; i < num_worker_threads; i++)
  {
    WorkerThread& worker = m_worker_threads.emplace_back(WorkerThread{this, i, nullptr});
    worker.thread = sthread_create(WorkerThreadEntry, &worker);
    if (!worker.thread)
    {
      m_worker_threads.pop_back();
      break;
    }
  }

  return HasWorkerThreads();
}

bool CustomAssetLoader::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_worker_threads.size() == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

bool CustomAssetLoader::HasWorkerThreads() const
{
  return !m_worker_threads.empty();
}

void CustomAssetLoader::StopWorkerThreads()
{
  if (!HasWorkerThreads())
    return;

  // Signal worker threads to stop, and wake all of them.
  m_exit_flag.Set();
  retro_eventcount_notify(&m_worker_thread_wake);

  // Wait for worker threads to exit.
  for (WorkerThread& worker : m_worker_threads)
    Common::JoinThread(worker.thread);
  m_worker_threads.clear();
  m_exit_flag.Clear();
}

void CustomAssetLoader::WorkerThreadEntry(void* data)
{
  const WorkerThread* const worker = static_cast<const WorkerThread*>(data);
  worker->owner->WorkerThreadRun(worker->index);
}

void CustomAssetLoader::FreeBatch(quiesce_node_t* node)
{
  delete reinterpret_cast<Batch*>(node);
}

void CustomAssetLoader::WorkerThreadRun(u32 thread_index)
{
#if defined(__LIBRETRO__) && defined(SKIP_ASSETLOADER_THREAD)
  return;
#endif
  Common::SetCurrentThreadName(fmt::format("Asset Loader {}", thread_index).c_str());

  while (!m_exit_flag.IsSet())
  {
    // Claim the next asset of the current batch, if it has one left.
    CustomAsset* item = nullptr;
    quiesce_enter(&m_batch_quiesce);
    if (auto* batch = static_cast<Batch*>(retro_atomic_load_acquire_ptr(&m_batch)))
    {
      // If more memory than allowed has already been loaded, we will load nothing more
      //  until the next ScheduleAssetsToLoad from Manager.
      if (m_change_in_memory > batch->allowed_memory)
        retro_atomic_store_release_size(&batch->cursor, batch->assets.size());

      const size_t index = retro_atomic_fetch_add_size(&batch->cursor, 1);
      if (index < batch->assets.size())
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
        has_work |= retro_atomic_load_acquire_size(const_cast<retro_atomic_size_t*>(&batch->cursor)) <
                    batch->assets.size();
      quiesce_exit(&m_batch_quiesce);
      if (has_work)
        retro_eventcount_cancel_wait(&m_worker_thread_wake);
      else
        retro_eventcount_commit_wait(&m_worker_thread_wake, key);
      continue;
    }

    // Unload previously loaded asset. An asset another thread is busy with comes back 0
    // and is skipped; the Manager schedules it again.
    m_change_in_memory -= item->Unload();

    const std::size_t bytes_loaded = item->Load();
    m_change_in_memory += s64(bytes_loaded);

    INFO_LOG_FMT(VIDEO, "CustomAssetLoader thread {} loaded: {} ({})", thread_index,
                 item->GetAssetId(), UICommon::FormatSize(bytes_loaded));

    auto* loaded = new Loaded{{}, {item->GetHandle(), bytes_loaded > 0}};
    mpsc_stack_push(&m_loaded, &loaded->link);
  }
}

auto CustomAssetLoader::TakeLoadResults() -> LoadResults
{
  LoadResults results{{}, 0};
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_loaded)); node;)
  {
    auto* loaded = reinterpret_cast<Loaded*>(node);
    node = node->next;
    results.asset_handles.push_back(loaded->handle);
    delete loaded;
  }
  results.change_in_memory = m_change_in_memory.exchange(0);
  return results;
}

void CustomAssetLoader::ScheduleAssetsToLoad(std::list<CustomAsset*> assets_to_load,
                                             u64 allowed_memory)
{
  if (assets_to_load.empty()) [[unlikely]]
    return;

  // There's new assets to process, publish them and notify worker threads.
  auto* batch = new Batch{{}, {assets_to_load.begin(), assets_to_load.end()},
                          static_cast<s64>(allowed_memory), {}};
  retro_atomic_size_init(&batch->cursor, 0);
  if (void* old = retro_atomic_exchange_ptr(&m_batch, batch))
    quiesce_retire(&m_batch_quiesce, &static_cast<Batch*>(old)->node, FreeBatch);
  retro_eventcount_notify(&m_worker_thread_wake);
}

void CustomAssetLoader::Reset(bool restart_worker_threads)
{
  const std::size_t worker_thread_count = m_worker_threads.size();
  StopWorkerThreads();

  // No worker runs now.
  delete static_cast<Batch*>(retro_atomic_exchange_ptr(&m_batch, nullptr));
  quiesce_deinit(&m_batch_quiesce);
  TakeLoadResults();
  m_change_in_memory = 0;

  if (restart_worker_threads)
  {
    StartWorkerThreads(static_cast<u32>(worker_thread_count));
  }
}

}  // namespace VideoCommon
