// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stddef.h>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"

/* A shader or pipeline to compile off the video thread. The owner embeds
 * this first in its own item and keeps the items; the compiler only moves
 * them between its queues. */
struct AsyncShaderWork
{
  mpsc_stack_node_t link; /* first: a drained chain is cast back */
  /* on a worker thread, or on the video thread when there are none */
  void (*compile)(AsyncShaderWork* work);
  /* on the video thread: takes the item back, to insert what it compiled
   * and release it, or to queue it again */
  void (*retrieve)(AsyncShaderWork* work);
  /* on the video thread: takes the item back unretrieved */
  void (*discard)(AsyncShaderWork* work);
  u32 priority;
  u32 seq; /* the order queued, among equal priorities */
};

namespace VideoCommon
{
class AsyncShaderCompiler final
{
public:
  AsyncShaderCompiler();
  ~AsyncShaderCompiler();

  AsyncShaderCompiler(const AsyncShaderCompiler&) = delete;
  AsyncShaderCompiler& operator=(const AsyncShaderCompiler&) = delete;

  // Queues a work item to the compiler threads. The lower the priority, the sooner
  // it is compiled, relative to the other work items.
  void QueueWorkItem(AsyncShaderWork* work, u32 priority);
  void RetrieveWorkItems();
  bool HasPendingWork();
  bool HasCompletedWork() const;

  // Discards both pending and completed work.
  void ClearAllWork();

  // Waits until every queued item is compiled. Returns false if interrupted.
  bool WaitUntilCompletion();

  bool StartWorkerThreads(u32 num_worker_threads);
  // Without worker threads, queued items wait and RetrieveWorkItems compiles a few of them each
  // time it is called, for a backend whose compiles only start work the driver does on threads
  // of its own (its pipelines then tell when they are ready, AbstractPipeline::IsReady).
  void SetDeferred(bool deferred) { m_deferred = deferred; }
  bool IsDeferred() const { return m_deferred && !HasWorkerThreads(); }
  bool ResizeWorkerThreads(u32 num_worker_threads);
  bool HasWorkerThreads() const { return m_num_workers != 0; }
  void StopWorkerThreads();

private:
  // A worker holds up to SLOTS items handed over by the video thread: one it may be
  // compiling and one to start right after, so it seldom idles until the next hand-over.
  enum
  {
    SLOTS = 2
  };
  struct Worker
  {
    AsyncShaderCompiler* owner;
    sthread_t* thread;
    retro_atomic_ptr_t slots[SLOTS];  // AsyncShaderWork*, written by the video thread
    retro_eventcount_t wake;          // when a slot fills or on exit
  };

  // Waits for a worker to finish an item; true once nothing is pending.
  bool WaitForWorkDone();

  static void WorkerThreadStart(void* worker);
  void WorkerThreadRun(Worker* worker);
  // Hands the most urgent pending items to the workers' empty slots. Video thread only.
  void FeedWorkers();
  size_t ItemsInSlots() const;

  // The pending items, a binary heap by (priority, seq). Video thread only.
  bool PendingPush(AsyncShaderWork* work);
  AsyncShaderWork* PendingPop();

  retro_atomic_int_t m_exit_flag;

  Worker* m_workers = nullptr;
  u32 m_num_workers = 0;

  AsyncShaderWork** m_pending = nullptr;
  size_t m_num_pending = 0;
  size_t m_pending_cap = 0;
  u32 m_seq = 0;

  retro_atomic_int_t m_busy_workers;
  // Notified each time a worker finishes an item.
  retro_eventcount_t m_work_done;

  mpsc_stack_t m_completed_work;
  // Items finished synchronously, when there are no workers, in order. Video thread only.
  AsyncShaderWork* m_completed_sync_head = nullptr;
  AsyncShaderWork* m_completed_sync_tail = nullptr;
  // How many waiting items each RetrieveWorkItems compiles, deferred.
  enum
  {
    DEFERRED_ITEMS_PER_RETRIEVE = 2
  };
  bool m_deferred = false;
};

}  // namespace VideoCommon
