// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"
#include "Common/Event.h"

namespace VideoCommon
{
class AsyncShaderCompiler
{
public:
  class WorkItem
  {
  public:
    virtual ~WorkItem() = default;
    virtual bool Compile() = 0;
    virtual void Retrieve() = 0;
  };

  using WorkItemPtr = std::unique_ptr<WorkItem>;

  AsyncShaderCompiler();
  virtual ~AsyncShaderCompiler();

  template <typename T, typename... Params>
  static WorkItemPtr CreateWorkItem(Params&&... params)
  {
    return std::make_unique<T>(std::forward<Params>(params)...);
  }

  // Queues a new work item to the compiler threads. The lower the priority, the sooner
  // this work item will be compiled, relative to the other work items.
  void QueueWorkItem(WorkItemPtr item, u32 priority);
  void RetrieveWorkItems();
  bool HasPendingWork();
  bool HasCompletedWork();

  // Clears both pending and completed work
  void ClearAllWork();

  // Calls progress_callback periodically, with completed_items, and total_items.
  // Returns false if interrupted.
  bool WaitUntilCompletion(const std::function<void(size_t, size_t)>& progress_callback);

  // Needed because of calling virtual methods in shutdown procedure.
  bool StartWorkerThreads(u32 num_worker_threads);
  bool ResizeWorkerThreads(u32 num_worker_threads);
  bool HasWorkerThreads() const;
  void StopWorkerThreads();

protected:
  virtual bool WorkerThreadInitMainThread(void** param);
  virtual bool WorkerThreadInitWorkerThread(void* param);
  virtual void WorkerThreadExit(void* param);

private:
  // Waits up to timeout_us for a worker to finish an item; true once nothing is pending.
  bool WaitForWorkDone(s64 timeout_us);

  void WorkerThreadStart();
  void WorkerThreadEntryPoint(void* param);
  void WorkerThreadRun();

  // A worker holds up to SLOTS items handed over by the video thread: one it may be
  // compiling and one to start right after, so it seldom idles until the next hand-over.
  static constexpr size_t SLOTS = 2;
  struct Worker
  {
    AsyncShaderCompiler* owner;
    void* param;
    sthread_t* thread = nullptr;
    std::array<retro_atomic_ptr_t, SLOTS> slots{};  // WorkItem*, written by the video thread
    retro_eventcount_t wake;                        // when a slot fills or on exit
  };

  // A finished item on its way back to the video thread.
  struct Completed
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    WorkItemPtr item;
  };

  static void WorkerThreadStart(void* worker);
  void WorkerThreadEntryPoint(Worker* worker);
  void WorkerThreadRun(Worker* worker);
  // Hands the most urgent pending items to the workers' empty slots. Video thread only.
  void FeedWorkers();
  size_t ItemsInSlots() const;

  retro_atomic_int_t m_exit_flag{};
  Common::Event m_init_event;
  retro_atomic_int_t m_worker_thread_start_result{};

  std::vector<std::unique_ptr<Worker>> m_workers;

  // Owned by the video thread alone. A multimap keeps the items in priority order and lets
  // the unique_ptr be moved out.
  std::multimap<u32, WorkItemPtr> m_pending_work;
  retro_atomic_int_t m_busy_workers{};
  // Notified each time a worker finishes an item.
  retro_eventcount_t m_work_done;

  mpsc_stack_t m_completed_work{};
  // Items finished synchronously, when there are no workers. Video thread only.
  std::vector<WorkItemPtr> m_completed_sync;
};

}  // namespace VideoCommon
