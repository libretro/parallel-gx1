// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include <net/net_compat.h>
#include <net/net_socket.h>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace ExpansionInterface
{
// The ports the instances on one machine take, one each.
static constexpr u16 IPC_BBA_FIRST_PORT = 47960;
static constexpr u16 IPC_BBA_PORTS = 8;

static void LoopbackAddress(sockaddr_in* addr, u16 port)
{
  std::memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

bool CEXIETHERNET::IPCBBAInterface::Activate()
{
  if (m_fd >= 0)
    return false;
  if (!network_init())
    return false;

  m_fd = socket_create("bba_ipc", SOCKET_DOMAIN_INET, SOCKET_TYPE_DATAGRAM, SOCKET_PROTOCOL_UDP);
  if (m_fd < 0)
    return false;

#ifdef _WIN32
  // An unreachable peer's ICMP reply would otherwise fail the next receive.
  {
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(m_fd, _WSAIOW(IOC_VENDOR, 12), &report, sizeof(report), nullptr, 0, &returned,
             nullptr, nullptr);
  }
#endif

  for (u16 i = 0; i < IPC_BBA_PORTS; i++)
  {
    sockaddr_in addr;
    LoopbackAddress(&addr, IPC_BBA_FIRST_PORT + i);
    if (bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
    {
      m_port = IPC_BBA_FIRST_PORT + i;
      break;
    }
  }
  if (!m_port)
  {
    ERROR_LOG_FMT(SP1, "BBA IPC: every port from {} to {} is taken", IPC_BBA_FIRST_PORT,
                  IPC_BBA_FIRST_PORT + IPC_BBA_PORTS - 1);
    socket_close(m_fd);
    m_fd = -1;
    return false;
  }
  INFO_LOG_FMT(SP1, "BBA IPC: on port {}", m_port);

  retro_atomic_store_release_int(&m_read_enabled, 0);
  retro_atomic_store_release_int(&m_read_thread_shutdown, 0);
  return RecvInit();
}

void CEXIETHERNET::IPCBBAInterface::Deactivate()
{
  if (m_fd < 0)
    return;
  retro_atomic_store_release_int(&m_read_enabled, 0);
  retro_atomic_store_release_int(&m_read_thread_shutdown, 1);

  if (m_read_thread)
  {
    // An empty datagram to ourselves ends the reader's receive.
    sockaddr_in self;
    LoopbackAddress(&self, m_port);
    sendto(m_fd, "", 0, 0, reinterpret_cast<const sockaddr*>(&self), sizeof(self));
    Common::JoinThread(m_read_thread);
    m_read_thread = nullptr;
  }

  socket_close(m_fd);
  m_fd = -1;
  m_port = 0;
}

bool CEXIETHERNET::IPCBBAInterface::IsActivated()
{
  return m_fd >= 0;
}

bool CEXIETHERNET::IPCBBAInterface::SendFrame(const u8* const frame, const u32 size)
{
  if (m_fd < 0)
    return false;

  for (u16 i = 0; i < IPC_BBA_PORTS; i++)
  {
    sockaddr_in peer;
    const u16 port = IPC_BBA_FIRST_PORT + i;
    if (port == m_port)
      continue;
    LoopbackAddress(&peer, port);
    // A port nobody holds drops the frame, as a segment with no one on it would.
    sendto(m_fd, reinterpret_cast<const char*>(frame), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
  }

  m_eth_ref->SendComplete();
  return true;
}

bool CEXIETHERNET::IPCBBAInterface::RecvInit()
{
  m_read_thread = sthread_create(
      Common::ThreadEntry<&CEXIETHERNET::IPCBBAInterface::ReadThreadHandler>, this);
  return m_read_thread != nullptr;
}

void CEXIETHERNET::IPCBBAInterface::RecvStart()
{
  retro_atomic_store_release_int(&m_read_enabled, 1);
}

void CEXIETHERNET::IPCBBAInterface::RecvStop()
{
  retro_atomic_store_release_int(&m_read_enabled, 0);
}

void CEXIETHERNET::IPCBBAInterface::ReadThreadHandler()
{
  // Frames are received straight into the adapter's receive buffer; one
  // that arrives while receiving is off is dropped.
  u8* const buffer = m_eth_ref->mRecvBuffer.get();
  for (;;)
  {
    const int size =
        static_cast<int>(recv(m_fd, reinterpret_cast<char*>(buffer), BBA_RECV_SIZE, 0));
    if (retro_atomic_load_acquire_int(&m_read_thread_shutdown))
      break;
    if (size <= 0 || !retro_atomic_load_acquire_int(&m_read_enabled))
      continue;
    m_eth_ref->mRecvBufferLength = static_cast<u32>(size);
    m_eth_ref->RecvHandlePacket();
  }
}

}  // namespace ExpansionInterface
