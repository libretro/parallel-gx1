// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/SocketContext.h"
#ifdef _WIN32
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#endif

namespace Common
{
#ifdef _WIN32
// Winsock counts WSAStartup calls itself and tears down at the matching last WSACleanup, so each
// context makes its own pair and nothing is shared between threads.
SocketContext::SocketContext()
{
  WSADATA data;
  const int ret = WSAStartup(MAKEWORD(2, 2), &data);
  if (ret == 0)
  {
    INFO_LOG(COMMON, "WSAStartup succeeded, wVersion=%d.%d, wHighVersion=%d.%d",
             int(LOBYTE(data.wVersion)), int(HIBYTE(data.wVersion)), int(LOBYTE(data.wHighVersion)),
             int(HIBYTE(data.wHighVersion)));
    m_started = true;
  }
  else
  {
    // The WSAStartup function directly returns the extended error code in the return value.
    // A call to the WSAGetLastError function is not needed and should not be used.
    //
    // Source:
    // https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsastartup
    ERROR_LOG(COMMON, "WSAStartup failed with error %d: %s", ret, Common::DecodeNetworkError(ret));
  }
}
SocketContext::~SocketContext()
{
  if (m_started)
    WSACleanup();
}

#else
SocketContext::SocketContext() = default;
SocketContext::~SocketContext() = default;
#endif
}  // namespace Common
