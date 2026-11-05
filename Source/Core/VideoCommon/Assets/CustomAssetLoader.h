// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <list>
#include <vector>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "Common/Quiesce.h"
#include "Common/Flag.h"
#include "VideoCommon/Assets/CustomAsset.h"

namespace VideoCommon
{
// This class takes any number of assets
// and loads them across a configurable
// thread pool
class CustomAssetLoader
{
public:
  CustomAssetLoader() { retro_eventcount_init(&m_worker_thread_wake); }
  ~CustomAssetLoader() { retro_eventcount_free(&m_worker_thread_wake); }
  CustomAssetLoader(const CustomAssetLoader&) = delete;
  CustomAssetLoader(CustomAssetLoader&&) = delete;
  CustomAssetLoader& operator=(const CustomAssetLoader&) = delete;
  CustomAssetLoader& operator=(CustomAssetLoader&&) = delete;

  void Initialize();
  void Shutdown();

  using AssetHandle = std::pair<std::size_t, bool>;
  struct LoadResults

  {
    std::vector<AssetHandle> asset_handles;
    s64 change_in_memory;
  };

  // Returns a vector of loaded asset handle / loaded result pairs
  //  and the change in memory.
  LoadResults TakeLoadResults();

  // Schedule assets to load on the worker threads
  //  and set how much memory is available for loading these additional assets.
  void ScheduleAssetsToLoad(std::list<CustomAsset*> assets_to_load, u64 allowed_memory);

  void Reset(bool restart_worker_threads = true);

private:
  bool StartWorkerThreads(u32 num_worker_threads);
  bool ResizeWorkerThreads(u32 num_worker_threads);
  bool HasWorkerThreads() const;
  void StopWorkerThreads();

  struct WorkerThread
  {
    CustomAssetLoader* owner;
    u32 index;
    sthread_t* thread;
  };
  static void WorkerThreadEntry(void* data);
  void WorkerThreadRun(u32 thread_index);

  Common::Flag m_exit_flag;

  std::vector<WorkerThread> m_worker_threads;

  // One ScheduleAssetsToLoad's assets, published whole. Workers claim the next one with an
  // atomic add on the cursor; a newer batch replaces it, and it is freed once no worker is
  // still looking at it.
  struct Batch
  {
    quiesce_node_t node;  // first: the free callback casts it back
    std::vector<CustomAsset*> assets;
    s64 allowed_memory;
    retro_atomic_size_t cursor;
  };
  static void FreeBatch(quiesce_node_t* node);
  quiesce_t m_batch_quiesce{};
  retro_atomic_ptr_t m_batch{};
  // Notified after a batch is published or the exit flag is set.
  retro_eventcount_t m_worker_thread_wake;

  // Results for TakeLoadResults, pushed by the workers.
  struct Loaded
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    AssetHandle handle;
  };
  mpsc_stack_t m_loaded{};

  // Change in memory from just-loaded/unloaded asset results yet to be taken by the Manager.
  std::atomic<s64> m_change_in_memory = 0;
};
}  // namespace VideoCommon
