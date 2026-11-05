// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <winsock2.h>

#endif

namespace Common
{
class SocketContext
{
public:
  SocketContext();
  ~SocketContext();

  SocketContext(const SocketContext&) = delete;
  SocketContext(SocketContext&&) = delete;

  SocketContext& operator=(const SocketContext&) = delete;
  SocketContext& operator=(SocketContext&&) = delete;

private:
#ifdef _WIN32
  bool m_started = false;
#endif
};
}  // namespace Common
