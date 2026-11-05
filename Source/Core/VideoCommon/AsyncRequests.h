// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <retro_atomic.h>
#include <retro_waitable_spsc.h>
#include <rthreads/retro_eventcount.h>

struct EfbPokeData;
class PointerWrap;

// Requests from the CPU thread to the video thread. The CPU thread is the only producer and the
// video thread the only consumer, so the requests travel through a lock-free SPSC ring as
// pointers: a blocking request lives on the caller's stack and allocates nothing.
class AsyncRequests
{
public:
  AsyncRequests();
  ~AsyncRequests();

  // Called from the Video thread.
  void PullEvents();

  // The following are called from the CPU thread.
  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    using Fn = std::decay_t<F>;
    struct Owned : Request
    {
      Fn fn;
      static void Run(Request* r)
      {
        Owned* const self = static_cast<Owned*>(r);
        std::invoke(self->fn);
        delete self;
      }
    };
    Submit(new Owned{{&Owned::Run}, Fn(std::forward<F>(callback))});
  }

  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    using R = std::invoke_result_t<F>;
    using Fn = std::remove_reference_t<F>;
    struct Blocking : Request
    {
      Fn* fn;
      retro_atomic_int_t done;
      std::conditional_t<std::is_void_v<R>, char, std::optional<R>> result;
      static void Run(Request* r)
      {
        Blocking* const self = static_cast<Blocking*>(r);
        if constexpr (std::is_void_v<R>)
          std::invoke(*self->fn);
        else
          self->result.emplace(std::invoke(*self->fn));
        // The caller owns this request again from here on.
        retro_atomic_store_release_int(&self->done, 1);
        retro_eventcount_notify(&s_singleton.m_completed);
      }
    };
    Blocking request;
    request.run = &Blocking::Run;
    request.fn = &callback;
    retro_atomic_int_init(&request.done, 0);
    Submit(&request);
    WaitDone(&request.done);
    if constexpr (!std::is_void_v<R>)
      return std::move(*request.result);
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  struct Request
  {
    void (*run)(Request*);
  };

  void Submit(Request* request);
  void WaitDone(retro_atomic_int_t* done);

  static AsyncRequests s_singleton;

  retro_waitable_spsc_t m_queue;
  retro_eventcount_t m_completed;
  bool m_passthrough = true;
};
