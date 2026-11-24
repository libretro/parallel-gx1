// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

namespace Common
{
template <typename Callable>
class ScopeGuard final
{
public:
  ScopeGuard(Callable&& finalizer) : m_finalizer(std::forward<Callable>(finalizer)), m_active(true)
  {
  }

  ScopeGuard(ScopeGuard&& other)
      : m_finalizer(std::move(other.m_finalizer)), m_active(other.m_active)
  {
    other.m_active = false;
  }

  ~ScopeGuard() { Exit(); }
  void Dismiss() { m_active = false; }
  void Exit()
  {
    if (!m_active)
      return;

    m_active = false;
    m_finalizer();  // must not throw
  }

  ScopeGuard(const ScopeGuard&) = delete;

  void operator=(const ScopeGuard&) = delete;

private:
  Callable m_finalizer;
  bool m_active;
};

}  // Namespace Common
