// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "Common/Flag.h"
#include "Common/Quiesce.h"
#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
// This class takes any number of assets
// and loads them across a configurable
// thread pool
class CustomAssetLoader
{
public:
  CustomAssetLoader() { retro_eventcount_init(&m_worker_thread_wake); }
  ~CustomAssetLoader();
  CustomAssetLoader(const CustomAssetLoader&) = delete;
  CustomAssetLoader(CustomAssetLoader&&) = delete;
  CustomAssetLoader& operator=(const CustomAssetLoader&) = delete;
  CustomAssetLoader& operator=(CustomAssetLoader&&) = delete;

  void Initialize();
  void Shutdown();

  // Calls on_loaded for each asset a worker finished loading since the last call, with
  // whether it loaded, and returns the change in memory the loads and unloads made.
  s64 TakeLoadResults(void (*on_loaded)(void* context, TextureAsset* asset, bool loaded),
                      void* context);

  // Schedule assets to load on the worker threads
  //  and set how much memory is available for loading these additional assets.
  void ScheduleAssetsToLoad(TextureAsset* const* assets, u32 count, u64 allowed_memory);

  void Reset(bool restart_worker_threads = true);

private:
  enum
  {
    MAX_WORKER_THREADS = 8
  };

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

  WorkerThread m_worker_threads[MAX_WORKER_THREADS] = {};
  u32 m_num_worker_threads = 0;

  // One ScheduleAssetsToLoad's assets, published whole. Workers claim the next one with an
  // atomic add on the cursor; a newer batch replaces it, and it goes back to the spares once
  // no worker is still looking at it, its array kept for the next schedule.
  struct Batch
  {
    quiesce_node_t node;  // first: the free callback casts it back
    CustomAssetLoader* owner;
    TextureAsset** assets;
    u32 count;
    u32 capacity;
    s64 allowed_memory;
    retro_atomic_size_t cursor;
  };
  static void RetireBatch(quiesce_node_t* node);
  quiesce_t m_batch_quiesce{};
  retro_atomic_ptr_t m_batch{};
  // Retired batches, pushed by whichever thread retires them; taken by the scheduling thread.
  mpsc_stack_t m_spare_batches{};
  // Notified after a batch is published or the exit flag is set.
  retro_eventcount_t m_worker_thread_wake;

  // Assets whose load finished, for TakeLoadResults, linked through the assets.
  mpsc_stack_t m_loaded{};

  // Change in memory from just-loaded/unloaded asset results yet to be taken by the Manager.
  retro_atomic_64_t m_change_in_memory{};
};
}  // namespace VideoCommon
