// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/AsyncShaderCompiler.h"

#include <thread>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "Core/Core.h"
#include "Core/System.h"

namespace VideoCommon
{
AsyncShaderCompiler::AsyncShaderCompiler()
{
}

AsyncShaderCompiler::~AsyncShaderCompiler()
{
  // Pending work can be left at shutdown.
  // The work item classes are expected to clean up after themselves.
  ASSERT(!HasWorkerThreads());
  ClearAllWork();
}

void AsyncShaderCompiler::QueueWorkItem(WorkItemPtr item, u32 priority)
{
  // If no worker threads are available, compile synchronously.
  if (!HasWorkerThreads())
  {
    item->Compile();
    m_completed_sync.push_back(std::move(item));
  }
  else
  {
    m_pending_work.emplace(priority, std::move(item));
    FeedWorkers();
  }
}

void AsyncShaderCompiler::FeedWorkers()
{
  for (auto& worker : m_workers)
  {
    if (m_pending_work.empty())
      return;
    bool filled = false;
    for (auto& slot : worker->slots)
    {
      if (m_pending_work.empty())
        break;
      if (retro_atomic_load_acquire_ptr(&slot))
        continue;
      auto iter = m_pending_work.begin();
      // The worker clears a slot only after taking its item, so an empty one stays empty
      // until this thread fills it.
      retro_atomic_store_release_ptr(&slot, iter->second.release());
      m_pending_work.erase(iter);
      filled = true;
    }
    if (filled)
      retro_eventcount_notify(&worker->wake);
  }
}

size_t AsyncShaderCompiler::ItemsInSlots() const
{
  size_t count = 0;
  for (const auto& worker : m_workers)
  {
    for (const auto& slot : worker->slots)
      count += retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&slot)) != nullptr;
  }
  return count;
}

void AsyncShaderCompiler::RetrieveWorkItems()
{
  // Workers that finished take their next items now, before this thread runs the retrievals.
  FeedWorkers();

  std::vector<WorkItemPtr> sync_work;
  sync_work.swap(m_completed_sync);
  for (auto& item : sync_work)
    item->Retrieve();

  // The stack hands them back newest first; retrieve them in the order they finished.
  mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_completed_work));
  while (node)
  {
    Completed* done = reinterpret_cast<Completed*>(node);
    node = node->next;
    done->item->Retrieve();
    delete done;
  }
}

bool AsyncShaderCompiler::HasPendingWork()
{
  FeedWorkers();
  return !m_pending_work.empty() || ItemsInSlots() != 0 ||
         retro_atomic_load_acquire_int(&m_busy_workers) != 0;
}

bool AsyncShaderCompiler::HasCompletedWork()
{
  return !m_completed_sync.empty() || !mpsc_stack_empty(&m_completed_work);
}

void AsyncShaderCompiler::ClearAllWork()
{
  m_pending_work.clear();

  for (auto& worker : m_workers)
  {
    for (auto& slot : worker->slots)
      delete static_cast<WorkItem*>(retro_atomic_exchange_ptr(&slot, nullptr));
  }

  m_completed_sync.clear();
  for (mpsc_stack_node_t* node = mpsc_stack_drain(&m_completed_work); node;)
  {
    Completed* done = reinterpret_cast<Completed*>(node);
    node = node->next;
    delete done;
  }
}

bool AsyncShaderCompiler::WaitUntilCompletion(
    const std::function<void(size_t, size_t)>& progress_callback)
{
  if (!HasPendingWork())
    return true;

  // Wait a second before opening a progress dialog.
  // This way, if the operation completes quickly, we don't annoy the user.
  constexpr u32 CHECK_INTERVAL_MS = 1000 / 30;
  constexpr auto CHECK_INTERVAL = std::chrono::milliseconds(CHECK_INTERVAL_MS);
  for (u32 i = 0; i < (1000 / CHECK_INTERVAL_MS); i++)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL));
    if (!HasPendingWork())
      return true;
  }

  // Grab the number of pending items. We use this to work out how many are left.
  const size_t total_items = m_completed_sync.size() + m_pending_work.size() + ItemsInSlots() +
                            retro_atomic_load_acquire_int(&m_busy_workers) + 1;

  // Update progress while the compiles complete.
  while (Core::GetState(Core::System::GetInstance()) != Core::State::Stopping)
  {
    if (!HasPendingWork())
      return true;
    const size_t remaining_items = m_pending_work.size() + ItemsInSlots();

    progress_callback(total_items - remaining_items, total_items);
    std::this_thread::sleep_for(CHECK_INTERVAL);
  }
  return false;
}

bool AsyncShaderCompiler::StartWorkerThreads(u32 num_worker_threads)
{
  if (num_worker_threads == 0)
    return true;

  for (u32 i = 0; i < num_worker_threads; i++)
  {
    void* thread_param = nullptr;
    if (!WorkerThreadInitMainThread(&thread_param))
    {
      WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker thread.");
      break;
    }

    retro_atomic_store_release_int(&m_worker_thread_start_result, 0);

    auto worker = std::make_unique<Worker>();
    worker->owner = this;
    worker->param = thread_param;
    retro_eventcount_init(&worker->wake);
    worker->thread = sthread_create(&AsyncShaderCompiler::WorkerThreadStart, worker.get());
    if (!worker->thread)
    {
      retro_eventcount_free(&worker->wake);
      WorkerThreadExit(thread_param);
      WARN_LOG_FMT(VIDEO, "Failed to start shader compiler worker thread.");
      break;
    }
    m_init_event.Wait();

    if (!retro_atomic_load_acquire_int(&m_worker_thread_start_result))
    {
      WARN_LOG_FMT(VIDEO, "Failed to start shader compiler worker thread.");
      Common::JoinThread(worker->thread);
      retro_eventcount_free(&worker->wake);
      break;
    }

    m_workers.push_back(std::move(worker));
  }

  return HasWorkerThreads();
}

bool AsyncShaderCompiler::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_workers.size() == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

bool AsyncShaderCompiler::HasWorkerThreads() const
{
  return !m_workers.empty();
}

void AsyncShaderCompiler::StopWorkerThreads()
{
  if (!HasWorkerThreads())
    return;

  // Signal worker threads to stop, and wake all of them.
  retro_atomic_store_release_int(&m_exit_flag, 1);
  for (auto& worker : m_workers)
    retro_eventcount_notify(&worker->wake);

  // Wait for worker threads to exit. What they still held goes back to the pending queue.
  for (auto& worker : m_workers)
  {
    Common::JoinThread(worker->thread);
    retro_eventcount_free(&worker->wake);
    for (auto& slot : worker->slots)
    {
      if (auto* item = static_cast<WorkItem*>(retro_atomic_exchange_ptr(&slot, nullptr)))
        m_pending_work.emplace(0u, WorkItemPtr(item));
    }
  }
  m_workers.clear();
  retro_atomic_store_release_int(&m_exit_flag, 0);
}

bool AsyncShaderCompiler::WorkerThreadInitMainThread(void** param)
{
  return true;
}

bool AsyncShaderCompiler::WorkerThreadInitWorkerThread(void* param)
{
  return true;
}

void AsyncShaderCompiler::WorkerThreadExit(void* param)
{
}

void AsyncShaderCompiler::WorkerThreadStart(void* worker)
{
  Worker* const w = static_cast<Worker*>(worker);
  w->owner->WorkerThreadEntryPoint(w);
}

void AsyncShaderCompiler::WorkerThreadEntryPoint(Worker* worker)
{
  Common::SetCurrentThreadName("AsyncShaderCompiler Worker");

  // Initialize worker thread with backend-specific method.
  if (!WorkerThreadInitWorkerThread(worker->param))
  {
    WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker.");
    retro_atomic_store_release_int(&m_worker_thread_start_result, 0);
    m_init_event.Set();
    return;
  }

  retro_atomic_store_release_int(&m_worker_thread_start_result, 1);
  m_init_event.Set();

  WorkerThreadRun(worker);

  WorkerThreadExit(worker->param);
}

void AsyncShaderCompiler::WorkerThreadRun(Worker* worker)
{
  size_t next = 0;
  for (;;)
  {
    if (retro_atomic_load_acquire_int(&m_exit_flag))
      return;

    // Take the slots in turn, so items run in the order they were handed over.
    WorkItem* item = nullptr;
    for (size_t i = 0; i < SLOTS && !item; ++i)
    {
      retro_atomic_ptr_t* slot = &worker->slots[(next + i) % SLOTS];
      if (retro_atomic_load_acquire_ptr(slot))
      {
        // Busy before the slot empties, so the video thread never sees neither.
        retro_atomic_fetch_add_int(&m_busy_workers, 1);
        item = static_cast<WorkItem*>(retro_atomic_exchange_ptr(slot, nullptr));
        if (!item)  // cleared by ClearAllWork meanwhile
          retro_atomic_fetch_sub_int(&m_busy_workers, 1);
        next = (next + i + 1) % SLOTS;
      }
    }

    if (!item)
    {
      const int key = retro_eventcount_prepare_wait(&worker->wake);
      bool any = retro_atomic_load_acquire_int(&m_exit_flag) != 0;
      for (auto& slot : worker->slots)
        any |= retro_atomic_load_acquire_ptr(&slot) != nullptr;
      if (any)
        retro_eventcount_cancel_wait(&worker->wake);
      else
        retro_eventcount_commit_wait(&worker->wake, key);
      continue;
    }

    WorkItemPtr owned(item);
    if (owned->Compile())
    {
      auto* done = new Completed{{}, std::move(owned)};
      mpsc_stack_push(&m_completed_work, &done->link);
    }
    retro_atomic_fetch_sub_int(&m_busy_workers, 1);
  }
}

}  // namespace VideoCommon
