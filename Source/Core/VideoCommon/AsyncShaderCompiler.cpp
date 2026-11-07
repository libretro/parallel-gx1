// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/AsyncShaderCompiler.h"

#include <stdlib.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "Core/Core.h"
#include "Core/System.h"

namespace VideoCommon
{
AsyncShaderCompiler::AsyncShaderCompiler()
{
  retro_atomic_int_init(&m_exit_flag, 0);
  retro_atomic_int_init(&m_busy_workers, 0);
  retro_eventcount_init(&m_work_done);
  mpsc_stack_init(&m_completed_work);
}

AsyncShaderCompiler::~AsyncShaderCompiler()
{
  // Pending work can be left at shutdown; its owner takes it back.
  ASSERT(!HasWorkerThreads());
  ClearAllWork();
  free(m_pending);
  retro_eventcount_free(&m_work_done);
}

/* the heap's order: lower priority first, then the one queued first */
static bool WorkBefore(const AsyncShaderWork* a, const AsyncShaderWork* b)
{
  if (a->priority != b->priority)
    return a->priority < b->priority;
  return static_cast<s32>(a->seq - b->seq) < 0;
}

bool AsyncShaderCompiler::PendingPush(AsyncShaderWork* work)
{
  size_t i;
  if (m_num_pending == m_pending_cap)
  {
    const size_t cap = m_pending_cap ? m_pending_cap * 2 : 256;
    AsyncShaderWork** grown =
        static_cast<AsyncShaderWork**>(realloc(m_pending, cap * sizeof(*grown)));
    if (!grown)
      return false;
    m_pending = grown;
    m_pending_cap = cap;
  }
  i = m_num_pending++;
  while (i)
  {
    const size_t parent = (i - 1) / 2;
    if (!WorkBefore(work, m_pending[parent]))
      break;
    m_pending[i] = m_pending[parent];
    i = parent;
  }
  m_pending[i] = work;
  return true;
}

AsyncShaderWork* AsyncShaderCompiler::PendingPop()
{
  AsyncShaderWork* top;
  AsyncShaderWork* last;
  size_t i = 0;
  if (!m_num_pending)
    return nullptr;
  top = m_pending[0];
  last = m_pending[--m_num_pending];
  for (;;)
  {
    size_t child = i * 2 + 1;
    if (child >= m_num_pending)
      break;
    if (child + 1 < m_num_pending && WorkBefore(m_pending[child + 1], m_pending[child]))
      child++;
    if (!WorkBefore(m_pending[child], last))
      break;
    m_pending[i] = m_pending[child];
    i = child;
  }
  if (m_num_pending)
    m_pending[i] = last;
  return top;
}

bool AsyncShaderCompiler::WaitForWorkDone()
{
  const int key = retro_eventcount_prepare_wait(&m_work_done);
  if (!HasPendingWork())
  {
    retro_eventcount_cancel_wait(&m_work_done);
    return true;
  }
  retro_eventcount_commit_wait(&m_work_done, key);
  return !HasPendingWork();
}

void AsyncShaderCompiler::QueueWorkItem(AsyncShaderWork* work, u32 priority)
{
  work->priority = priority;
  work->seq = m_seq++;
  work->link.next = nullptr;

  // If no worker threads are available, compile synchronously, or later when deferred.
  if (!IsDeferred() && !HasWorkerThreads())
  {
    work->compile(work);
    if (m_completed_sync_tail)
      m_completed_sync_tail->link.next = &work->link;
    else
      m_completed_sync_head = work;
    m_completed_sync_tail = work;
    return;
  }

  if (!PendingPush(work))
  {
    /* no room to wait in: compiled here, as without workers */
    work->compile(work);
    work->retrieve(work);
    return;
  }
  if (!IsDeferred())
    FeedWorkers();
}

void AsyncShaderCompiler::FeedWorkers()
{
  u32 w;
  for (w = 0; w < m_num_workers && m_num_pending; w++)
  {
    Worker* worker = &m_workers[w];
    bool filled = false;
    size_t s;
    for (s = 0; s < SLOTS && m_num_pending; s++)
    {
      if (retro_atomic_load_acquire_ptr(&worker->slots[s]))
        continue;
      // The worker clears a slot only after taking its item, so an empty one stays empty
      // until this thread fills it.
      retro_atomic_store_release_ptr(&worker->slots[s], PendingPop());
      filled = true;
    }
    if (filled)
      retro_eventcount_notify(&worker->wake);
  }
}

size_t AsyncShaderCompiler::ItemsInSlots() const
{
  size_t count = 0;
  u32 w;
  for (w = 0; w < m_num_workers; w++)
  {
    size_t s;
    for (s = 0; s < SLOTS; s++)
      count += retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&m_workers[w].slots[s])) !=
               nullptr;
  }
  return count;
}

void AsyncShaderCompiler::RetrieveWorkItems()
{
  mpsc_stack_node_t* node;

  if (IsDeferred())
  {
    /* the most urgent few; an item may queue others, which wait their turn */
    size_t i;
    for (i = 0; i < DEFERRED_ITEMS_PER_RETRIEVE && m_num_pending; i++)
    {
      AsyncShaderWork* work = PendingPop();
      work->compile(work);
      work->retrieve(work);
    }
    return;
  }

  // Workers that finished take their next items now, before this thread runs the retrievals.
  FeedWorkers();

  /* those a retrieval queues wait for the next call */
  if (m_completed_sync_head)
  {
    node = &m_completed_sync_head->link;
    m_completed_sync_head = nullptr;
    m_completed_sync_tail = nullptr;
    while (node)
    {
      AsyncShaderWork* work = reinterpret_cast<AsyncShaderWork*>(node);
      node = node->next;
      work->retrieve(work);
    }
  }

  // The stack hands them back newest first; retrieve them in the order they finished.
  node = mpsc_stack_reverse(mpsc_stack_drain(&m_completed_work));
  while (node)
  {
    AsyncShaderWork* work = reinterpret_cast<AsyncShaderWork*>(node);
    node = node->next;
    work->retrieve(work);
  }
}

bool AsyncShaderCompiler::HasPendingWork()
{
  FeedWorkers();
  return m_num_pending || ItemsInSlots() != 0 ||
         retro_atomic_load_acquire_int(&m_busy_workers) != 0;
}

bool AsyncShaderCompiler::HasCompletedWork() const
{
  return m_completed_sync_head ||
         !mpsc_stack_empty(const_cast<mpsc_stack_t*>(&m_completed_work));
}

void AsyncShaderCompiler::ClearAllWork()
{
  mpsc_stack_node_t* node;
  u32 w;

  while (m_num_pending)
  {
    AsyncShaderWork* work = PendingPop();
    work->discard(work);
  }

  for (w = 0; w < m_num_workers; w++)
  {
    size_t s;
    for (s = 0; s < SLOTS; s++)
    {
      AsyncShaderWork* work =
          static_cast<AsyncShaderWork*>(retro_atomic_exchange_ptr(&m_workers[w].slots[s], nullptr));
      if (work)
        work->discard(work);
    }
  }

  node = m_completed_sync_head ? &m_completed_sync_head->link : nullptr;
  m_completed_sync_head = nullptr;
  m_completed_sync_tail = nullptr;
  while (node)
  {
    AsyncShaderWork* work = reinterpret_cast<AsyncShaderWork*>(node);
    node = node->next;
    work->discard(work);
  }

  node = mpsc_stack_drain(&m_completed_work);
  while (node)
  {
    AsyncShaderWork* work = reinterpret_cast<AsyncShaderWork*>(node);
    node = node->next;
    work->discard(work);
  }
}

bool AsyncShaderCompiler::WaitUntilCompletion()
{
  if (!HasPendingWork())
    return true;

  if (IsDeferred())
  {
    while (m_num_pending)
      RetrieveWorkItems();
    return true;
  }

  // Woken as each item finishes.
  while (Core::GetState(Core::System::GetInstance()) != Core::State::Stopping)
  {
    if (WaitForWorkDone())
      return true;
  }
  return false;
}

bool AsyncShaderCompiler::StartWorkerThreads(u32 num_worker_threads)
{
  u32 i;

  if (num_worker_threads == 0)
    return true;

  m_workers = static_cast<Worker*>(calloc(num_worker_threads, sizeof(*m_workers)));
  if (!m_workers)
    return false;

  for (i = 0; i < num_worker_threads; i++)
  {
    Worker* worker = &m_workers[m_num_workers];
    size_t s;
    worker->owner = this;
    for (s = 0; s < SLOTS; s++)
      retro_atomic_ptr_init(&worker->slots[s], nullptr);
    retro_eventcount_init(&worker->wake);
    worker->thread = sthread_create(&AsyncShaderCompiler::WorkerThreadStart, worker);
    if (!worker->thread)
    {
      retro_eventcount_free(&worker->wake);
      WARN_LOG_FMT(VIDEO, "Failed to start shader compiler worker thread.");
      break;
    }
    m_num_workers++;
  }

  if (!m_num_workers)
  {
    free(m_workers);
    m_workers = nullptr;
  }
  return HasWorkerThreads();
}

bool AsyncShaderCompiler::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_num_workers == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

void AsyncShaderCompiler::StopWorkerThreads()
{
  u32 w;

  if (!HasWorkerThreads())
    return;

  // Signal worker threads to stop, and wake all of them.
  retro_atomic_store_release_int(&m_exit_flag, 1);
  for (w = 0; w < m_num_workers; w++)
    retro_eventcount_notify(&m_workers[w].wake);

  // Wait for worker threads to exit. What they still held goes back to the pending queue,
  // first in line.
  for (w = 0; w < m_num_workers; w++)
  {
    Worker* worker = &m_workers[w];
    size_t s;
    Common::JoinThread(worker->thread);
    retro_eventcount_free(&worker->wake);
    for (s = 0; s < SLOTS; s++)
    {
      AsyncShaderWork* work =
          static_cast<AsyncShaderWork*>(retro_atomic_exchange_ptr(&worker->slots[s], nullptr));
      if (!work)
        continue;
      work->priority = 0;
      if (!PendingPush(work))
        work->discard(work);
    }
  }
  free(m_workers);
  m_workers = nullptr;
  m_num_workers = 0;
  retro_atomic_store_release_int(&m_exit_flag, 0);
}

void AsyncShaderCompiler::WorkerThreadStart(void* worker)
{
  Worker* const w = static_cast<Worker*>(worker);
  Common::SetCurrentThreadName("AsyncShaderCompiler Worker");
  /* what is compiled in the background waits; the frame being emulated
   * does not, on a machine with fewer cores than busy threads */
  Common::LowerCurrentThreadPriority();
  w->owner->WorkerThreadRun(w);
}

void AsyncShaderCompiler::WorkerThreadRun(Worker* worker)
{
  size_t next = 0;
  for (;;)
  {
    AsyncShaderWork* work = nullptr;
    size_t i;

    if (retro_atomic_load_acquire_int(&m_exit_flag))
      return;

    // Take the slots in turn, so items run in the order they were handed over.
    for (i = 0; i < SLOTS && !work; ++i)
    {
      retro_atomic_ptr_t* slot = &worker->slots[(next + i) % SLOTS];
      if (retro_atomic_load_acquire_ptr(slot))
      {
        // Busy before the slot empties, so the video thread never sees neither.
        retro_atomic_fetch_add_int(&m_busy_workers, 1);
        work = static_cast<AsyncShaderWork*>(retro_atomic_exchange_ptr(slot, nullptr));
        if (!work)  // cleared by ClearAllWork meanwhile
          retro_atomic_fetch_sub_int(&m_busy_workers, 1);
        next = (next + i + 1) % SLOTS;
      }
    }

    if (!work)
    {
      const int key = retro_eventcount_prepare_wait(&worker->wake);
      bool any = retro_atomic_load_acquire_int(&m_exit_flag) != 0;
      size_t s;
      for (s = 0; s < SLOTS; s++)
        any |= retro_atomic_load_acquire_ptr(&worker->slots[s]) != nullptr;
      if (any)
        retro_eventcount_cancel_wait(&worker->wake);
      else
        retro_eventcount_commit_wait(&worker->wake, key);
      continue;
    }

    work->compile(work);
    mpsc_stack_push(&m_completed_work, &work->link);
    retro_atomic_fetch_sub_int(&m_busy_workers, 1);
    retro_eventcount_notify(&m_work_done);
  }
}

}  // namespace VideoCommon
