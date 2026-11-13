// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <utility>

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

namespace Common
{
namespace detail
{
// A worker thread fed by a lock-free queue. Producers push onto an mpsc_stack, so any number of
// threads may push without a lock; the worker takes the whole stack at once and runs it in push
// order. Items are moved into their queue node on push and moved out of it once, by the worker.
//
// The control calls (Reset, Cancel, Shutdown, Stop, WaitForCompletion) are made from one thread.
template <typename T, bool IsSingleProducer>
class WorkQueueThreadBase final
{
public:
  using FunctionType = std::function<void(T)>;

  WorkQueueThreadBase()
  {
    mpsc_stack_init(&m_items);
    mpsc_stack_init(&m_free);
    retro_atomic_size_init(&m_pushed, 0);
    retro_atomic_size_init(&m_done, 0);
    retro_atomic_size_init(&m_cancel_to, 0);
    retro_atomic_int_init(&m_stop, 0);
    retro_eventcount_init(&m_wake);
    retro_eventcount_init(&m_progress);
  }
  WorkQueueThreadBase(std::string name, FunctionType function) : WorkQueueThreadBase()
  {
    Reset(std::move(name), std::move(function));
  }
  ~WorkQueueThreadBase()
  {
    Shutdown();
    FreeChain(m_held);
    FreeChain(reinterpret_cast<Node*>(mpsc_stack_reverse(mpsc_stack_drain(&m_items))));
    FreeChain(m_producer_free);
    FreeChain(reinterpret_cast<Node*>(mpsc_stack_drain(&m_free)));
    retro_eventcount_free(&m_progress);
    retro_eventcount_free(&m_wake);
  }

  WorkQueueThreadBase(const WorkQueueThreadBase&) = delete;
  WorkQueueThreadBase& operator=(const WorkQueueThreadBase&) = delete;

  // Shuts the current work thread down (if any) and starts a new thread with the given function.
  // Items pushed before the thread starts are kept and run by it.
  void Reset(std::string name, FunctionType function)
  {
    Shutdown();
    m_name = std::move(name);
    m_function = std::move(function);
    m_thread = sthread_create(ThreadEntry, this);
  }

  // Adds an item to the work queue. Never blocks.
  template <typename... Args>
  void EmplaceItem(Args&&... args)
  {
    Node* node = nullptr;
    if constexpr (IsSingleProducer)
    {
      /* a node the worker is done with: the only producer takes them, so no other thread does */
      if (!m_producer_free)
        m_producer_free = reinterpret_cast<Node*>(mpsc_stack_drain(&m_free));
      node = m_producer_free;
      if (node)
      {
        m_producer_free = reinterpret_cast<Node*>(node->link.next);
        node->link.next = nullptr;
        node->value = T(std::forward<Args>(args)...);
      }
    }
    if (!node)
      node = new Node{{nullptr}, T(std::forward<Args>(args)...)};
    mpsc_stack_push(&m_items, &node->link);
    retro_atomic_fetch_add_size(&m_pushed, 1);
    retro_eventcount_notify(&m_wake);
  }
  void Push(T&& item) { EmplaceItem(std::move(item)); }
  void Push(const T& item) { EmplaceItem(item); }

  // Empties the queue, skipping all work.
  // Blocks until the current work is cancelled.
  void Cancel()
  {
    const std::size_t pushed = retro_atomic_load_acquire_size(&m_pushed);
    if (retro_atomic_load_acquire_size(&m_done) == pushed)
      return;

    if (!IsRunning())
    {
      // No worker: the queue is ours.
      Node* chain = TakeAll();
      std::size_t skipped = 0;
      while (chain)
      {
        Node* const next = reinterpret_cast<Node*>(chain->link.next);
        delete chain;
        chain = next;
        ++skipped;
      }
      retro_atomic_fetch_add_size(&m_done, skipped);
      return;
    }

    retro_atomic_store_release_size(&m_cancel_to, pushed);
    retro_eventcount_notify(&m_wake);
    WaitForDone(pushed);
  }

  // Tells the worker thread to stop when its queue is empty.
  // Blocks until the worker thread exits. Does nothing if thread isn't running.
  void Shutdown()
  {
    WaitForCompletion();
    StopThread();
  }

  // Tells the worker thread to stop immediately, potentially leaving work in the queue.
  // Blocks until the worker thread exits. Does nothing if thread isn't running.
  void Stop() { StopThread(); }

  // Stops the worker thread ASAP and empties the queue.
  void StopAndCancel()
  {
    Stop();
    Cancel();
  }

  // Blocks until all items in the queue have been processed (or cancelled)
  // Does nothing if thread isn't running.
  void WaitForCompletion()
  {
    if (IsRunning())
      WaitForDone(retro_atomic_load_acquire_size(&m_pushed));
  }

  bool IsRunning() const { return m_thread != nullptr; }

private:
  struct Node
  {
    mpsc_stack_node_t link;
    T value;
  };

  static void FreeChain(Node* chain)
  {
    while (chain)
    {
      Node* const next = reinterpret_cast<Node*>(chain->link.next);
      delete chain;
      chain = next;
    }
  }

  // Everything queued, oldest first: what a stopped worker held back, then the stack.
  Node* TakeAll()
  {
    Node* chain = reinterpret_cast<Node*>(mpsc_stack_reverse(mpsc_stack_drain(&m_items)));
    if (!m_held)
      return chain;
    Node* tail = m_held;
    while (tail->link.next)
      tail = reinterpret_cast<Node*>(tail->link.next);
    tail->link.next = chain ? &chain->link : nullptr;
    chain = m_held;
    m_held = nullptr;
    return chain;
  }

  void WaitForDone(std::size_t target)
  {
    while (retro_atomic_load_acquire_size(&m_done) < target)
    {
      const int key = retro_eventcount_prepare_wait(&m_progress);
      if (retro_atomic_load_acquire_size(&m_done) >= target)
      {
        retro_eventcount_cancel_wait(&m_progress);
        break;
      }
      retro_eventcount_commit_wait(&m_progress, key);
    }
  }

  void StopThread()
  {
    if (!m_thread)
      return;

    retro_atomic_store_release_int(&m_stop, 1);
    retro_eventcount_notify(&m_wake);
    sthread_join(m_thread);
    m_thread = nullptr;
    retro_atomic_store_release_int(&m_stop, 0);
  }

  static void ThreadEntry(void* data) { static_cast<WorkQueueThreadBase*>(data)->ThreadLoop(); }

  void ThreadLoop()
  {
    sthread_setname(m_name.c_str());

    // Items are numbered in the order they run; m_done is the last one finished.
    std::size_t seq = retro_atomic_load_relaxed_size(&m_done);
    Node* chain = m_held;
    m_held = nullptr;

    for (;;)
    {
      if (!chain)
        chain = reinterpret_cast<Node*>(mpsc_stack_reverse(mpsc_stack_drain(&m_items)));

      if (retro_atomic_load_acquire_int(&m_stop))
        break;

      if (chain)
      {
        Node* const node = chain;
        chain = reinterpret_cast<Node*>(node->link.next);
        ++seq;
        if (seq > retro_atomic_load_acquire_size(&m_cancel_to))
          m_function(std::move(node->value));
        if constexpr (IsSingleProducer)
          mpsc_stack_push(&m_free, &node->link);  // the producer's to take again
        else
          delete node;
        retro_atomic_store_release_size(&m_done, seq);
        retro_eventcount_notify(&m_progress);
        continue;
      }

      const int key = retro_eventcount_prepare_wait(&m_wake);
      if (!mpsc_stack_empty(&m_items) || retro_atomic_load_acquire_int(&m_stop))
      {
        retro_eventcount_cancel_wait(&m_wake);
        continue;
      }
      retro_eventcount_commit_wait(&m_wake, key);
    }

    // Stopped with work left: keep it, in order, for the next thread.
    m_held = chain;
  }

  sthread_t* m_thread = nullptr;
  Node* m_held = nullptr;  // worker-owned while running, ours while stopped
  FunctionType m_function;
  std::string m_name;
  mpsc_stack_t m_items;
  // With one producer: the nodes the worker is done with, and those the producer drained from them
  mpsc_stack_t m_free;
  Node* m_producer_free = nullptr;
  retro_eventcount_t m_wake;
  retro_eventcount_t m_progress;
  retro_atomic_size_t m_pushed;
  retro_atomic_size_t m_done;
  retro_atomic_size_t m_cancel_to;
  retro_atomic_int_t m_stop;
};

// A WorkQueueThread-like class that takes functions to invoke.
template <template <typename> typename WorkThread>
class AsyncWorkThreadBase
{
public:
  using FuncType = std::function<void()>;

  AsyncWorkThreadBase() { retro_eventcount_init(&m_ran); }
  explicit AsyncWorkThreadBase(std::string thread_name) : AsyncWorkThreadBase()
  {
    Reset(std::move(thread_name));
  }
  ~AsyncWorkThreadBase()
  {
    m_worker.Shutdown();
    retro_eventcount_free(&m_ran);
  }

  void Reset(std::string thread_name)
  {
    m_worker.Reset(std::move(thread_name), std::invoke<FuncType>);
  }

  void Push(FuncType func) { m_worker.Push(std::move(func)); }

  // Runs func on the worker and returns once it has run.
  void PushBlocking(FuncType func)
  {
    // The flag lives on this stack; the eventcount outlives the call, since the worker may
    // still be inside the notify when this returns.
    retro_atomic_int_t done;
    retro_atomic_int_init(&done, 0);
    m_worker.EmplaceItem([&func, &done, this] {
      func();
      retro_atomic_store_release_int(&done, 1);
      retro_eventcount_notify(&m_ran);
    });
    while (!retro_atomic_load_acquire_int(&done))
    {
      const int key = retro_eventcount_prepare_wait(&m_ran);
      if (retro_atomic_load_acquire_int(&done))
      {
        retro_eventcount_cancel_wait(&m_ran);
        break;
      }
      retro_eventcount_commit_wait(&m_ran, key);
    }
  }

  void Cancel() { m_worker.Cancel(); }
  void Shutdown() { m_worker.Shutdown(); }
  void WaitForCompletion() { m_worker.WaitForCompletion(); }

private:
  WorkThread<FuncType> m_worker;
  retro_eventcount_t m_ran;
};
}  // namespace detail

// Multiple threads may push.
template <typename T>
using WorkQueueThread = detail::WorkQueueThreadBase<T, false>;

// A "Single Producer" WorkQueueThread: only one thread pushes.
template <typename T>
using WorkQueueThreadSP = detail::WorkQueueThreadBase<T, true>;

using AsyncWorkThread = detail::AsyncWorkThreadBase<WorkQueueThread>;
using AsyncWorkThreadSP = detail::AsyncWorkThreadBase<WorkQueueThreadSP>;

}  // namespace Common
