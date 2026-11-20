// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CPUThreadConfigCallback.h"

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Core/Core.h"

#include <cstdlib>

#include <retro_atomic.h>

namespace
{
retro_atomic_int_t s_should_run_callbacks;

struct Callback
{
  CPUThreadConfigCallback::ConfigChangedCallbackID id;
  Config::ConfigChangedCallback func;
  void* user;
};
// the CPU thread's callbacks, in the order they were added
enum
{
  MAX_CALLBACKS = 64
};
Callback s_callbacks[MAX_CALLBACKS];
size_t s_num_callbacks = 0;

size_t s_next_callback_id = 0;

void RunCallbacks()
{
  for (size_t i = 0; i < s_num_callbacks; i++)
    s_callbacks[i].func(s_callbacks[i].user);
}

void OnConfigChanged(void*)
{
  if (Core::IsCPUThread())
  {
    retro_atomic_store_relaxed_int(&s_should_run_callbacks, 0);
    RunCallbacks();
  }
  else
  {
    retro_atomic_store_relaxed_int(&s_should_run_callbacks, 1);
  }
}

}  // namespace

namespace CPUThreadConfigCallback
{
ConfigChangedCallbackID AddConfigChangedCallback(Config::ConfigChangedCallback func, void* user)
{
  [[maybe_unused]] static auto s_config_changed_callback_id =
      Config::AddConfigChangedCallback(&OnConfigChanged, nullptr);

  const ConfigChangedCallbackID callback_id{s_next_callback_id};
  ++s_next_callback_id;
  // More live callbacks than MAX_CALLBACKS: a leak in the caller, not a load to grow for.
  if (s_num_callbacks == MAX_CALLBACKS)
    std::abort();
  s_callbacks[s_num_callbacks++] = Callback{callback_id, func, user};
  return callback_id;
}

void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id)
{
  for (size_t i = 0; i < s_num_callbacks; i++)
  {
    if (s_callbacks[i].id == callback_id)
    {
      for (; i + 1 < s_num_callbacks; i++)
        s_callbacks[i] = s_callbacks[i + 1];
      s_num_callbacks--;
      return;
    }
  }
}

void CheckForConfigChanges()
{
  DEBUG_ASSERT(Core::IsCPUThread());

  if (retro_atomic_exchange_int(&s_should_run_callbacks, 0))
    RunCallbacks();
}

}  // namespace CPUThreadConfigCallback
