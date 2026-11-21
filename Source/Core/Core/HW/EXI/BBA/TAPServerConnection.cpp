// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2ipdef.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/NetSocket.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"

namespace ExpansionInterface
{

#ifdef _WIN32
using ws_ssize_t = int;
#else
#define closesocket close
using ws_ssize_t = ssize_t;
#endif

using Common::SEND_FLAGS;

TAPServerConnection::TAPServerConnection(const char* destination, RecvCallback recv_cb,
                                         void* recv_user, std::size_t max_frame_size)
    : m_destination(strdup(destination ? destination : "")), m_recv_cb(recv_cb),
      m_recv_user(recv_user), m_max_frame_size(max_frame_size),
      m_frame_buffer(static_cast<u8*>(std::malloc(max_frame_size ? max_frame_size : 1)))
{
}

TAPServerConnection::~TAPServerConnection()
{
  // The read thread writes the frame buffer until it is joined.
  Deactivate();
  std::free(m_frame_buffer);
  std::free(m_destination);
}

static int ConnectToDestination(const char* destination)
{
  if (!destination || !destination[0])
  {
    ERROR_LOG(SP1, "Cannot connect: destination is empty\n");
    return -1;
  }

  int ss_size;
  sockaddr_storage ss;
  std::memset(&ss, 0, sizeof(ss));
  if (destination[0] != '/')
  {
    // IP address or hostname
    const char* const colon = std::strchr(destination, ':');
    if (!colon)
    {
      ERROR_LOG(SP1, "Destination IP address does not include port\n");
      return -1;
    }

    char host[256];
    const std::size_t host_len = static_cast<std::size_t>(colon - destination);
    if (host_len >= sizeof(host))
    {
      ERROR_LOG(SP1, "Destination IP address is not valid\n");
      return -1;
    }
    std::memcpy(host, destination, host_len);
    host[host_len] = '\0';

    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&ss);
    uint32_t dest_ip;
    if (!net_resolve_ipv4(host, &dest_ip))
    {
      ERROR_LOG(SP1, "Destination IP address is not valid\n");
      return -1;
    }
    sin->sin_addr.s_addr = dest_ip;
    sin->sin_family = AF_INET;
    const int dest_port = std::atoi(colon + 1);
    if (dest_port < 1 || dest_port > 65535)
    {
      ERROR_LOG(SP1, "Destination port is not valid\n");
      return -1;
    }
    sin->sin_port = htons(dest_port);
    ss_size = sizeof(*sin);
#ifndef _WIN32
  }
  else
  {
    // UNIX socket
    sockaddr_un* sun = reinterpret_cast<sockaddr_un*>(&ss);
    if (std::strlen(destination) + 1 > sizeof(sun->sun_path))
    {
      ERROR_LOG(SP1, "Socket path is too long; unable to create tapserver connection\n");
      return -1;
    }
    sun->sun_family = AF_UNIX;
    std::strcpy(sun->sun_path, destination);
    ss_size = sizeof(*sun);
#else
  }
  else
  {
    ERROR_LOG(SP1, "UNIX sockets are not supported on Windows\n");
    return -1;
#endif
  }

  const int fd = socket(ss.ss_family, SOCK_STREAM, (ss.ss_family == AF_INET) ? IPPROTO_TCP : 0);
  if (fd == -1)
  {
    ERROR_LOG(SP1, "Couldn't create socket; unable to create tapserver connection\n");
    return -1;
  }

  Common::SetPlatformSocketOptions(fd);

  if (connect(fd, reinterpret_cast<sockaddr*>(&ss), ss_size) == -1)
  {
    INFO_LOG(SP1, "Couldn't connect socket (%s), unable to create tapserver connection\n",
             Common::StrNetworkError());
    closesocket(fd);
    return -1;
  }

  return fd;
}

bool TAPServerConnection::Activate()
{
  if (IsActivated())
    return true;

  m_fd = ConnectToDestination(m_destination);
  if (m_fd < 0)
    return false;

  return RecvInit();
}

void TAPServerConnection::Deactivate()
{
  m_read_enabled.Clear();
  m_read_shutdown.Set();
  Common::JoinThread(m_read_thread);
  m_read_shutdown.Clear();

  if (m_fd >= 0)
    closesocket(m_fd);
  m_fd = -1;
}

bool TAPServerConnection::IsActivated()
{
  return (m_fd >= 0);
}

bool TAPServerConnection::RecvInit()
{
  m_read_thread = sthread_create(Common::ThreadEntry<&TAPServerConnection::ReadThreadHandler>, this);
  return true;
}

void TAPServerConnection::RecvStart()
{
  m_read_enabled.Set();
}

void TAPServerConnection::RecvStop()
{
  m_read_enabled.Clear();
}

bool TAPServerConnection::SendAndRemoveAllHDLCFrames(u8* send_buf, std::size_t* send_size)
{
  // Bytes before consumed have been sent, or skipped before a frame's start flag.
  std::size_t consumed = 0;
  bool ok = true;
  while (consumed != *send_size)
  {
    const u8* const begin = send_buf + consumed;
    const std::size_t remaining = *send_size - consumed;
    const u8* const start = static_cast<const u8*>(std::memchr(begin, 0x7E, remaining));
    if (!start)
      break;
    const std::size_t start_offset = static_cast<std::size_t>(start - send_buf);
    const u8* const end_sentinel =
        static_cast<const u8*>(std::memchr(start + 1, 0x7E, *send_size - start_offset - 1));
    if (!end_sentinel)
      break;
    const std::size_t end_sentinel_offset = static_cast<std::size_t>(end_sentinel - send_buf);
    const std::size_t end_offset = end_sentinel_offset + 1;
    const std::size_t size = end_offset - start_offset;

    const u8 size_bytes[2] = {static_cast<u8>(size), static_cast<u8>(size >> 8)};
    if (send(m_fd, reinterpret_cast<const char*>(size_bytes), 2, SEND_FLAGS) != 2)
    {
      ERROR_LOG(SP1, "SendAndRemoveAllHDLCFrames(): could not write size field");
      ok = false;
      break;
    }
    const int written_bytes = send(m_fd, reinterpret_cast<const char*>(send_buf + start_offset),
                                   static_cast<int>(size), SEND_FLAGS);
    if (u32(written_bytes) != size)
    {
      ERROR_LOG(SP1,
                "SendAndRemoveAllHDLCFrames(): expected to write %" PRIu64
                " bytes, instead wrote %d",
                (u64)size, written_bytes);
      ok = false;
      break;
    }
    consumed = end_offset;
  }
  if (consumed)
  {
    std::memmove(send_buf, send_buf + consumed, *send_size - consumed);
    *send_size -= consumed;
  }
  return ok;
}

bool TAPServerConnection::SendFrame(const u8* frame, u32 size)
{
  INFO_LOG(SP1, "SendFrame %u\n%s", size, ArrayToString(frame, size, 0x10).c_str());

  // On Windows, the data pointer is of type const char*; on other systems it is
  // of type const void*. This is the reason for the reinterpret_cast here and
  // in the other send/recv calls in this file.
  const u8 size_bytes[2] = {static_cast<u8>(size), static_cast<u8>(size >> 8)};
  if (send(m_fd, reinterpret_cast<const char*>(size_bytes), 2, SEND_FLAGS) != 2)
  {
    ERROR_LOG(SP1, "SendFrame(): could not write size field");
    return false;
  }
  const int written_bytes =
      send(m_fd, reinterpret_cast<const char*>(frame), static_cast<ws_ssize_t>(size), SEND_FLAGS);
  if (u32(written_bytes) != size)
  {
    ERROR_LOG(SP1, "SendFrame(): expected to write %u bytes, instead wrote %d", size,
              written_bytes);
    return false;
  }
  return true;
}

void TAPServerConnection::ReadThreadHandler()
{
  enum class ReadState
  {
    SIZE,
    SIZE_HIGH,
    DATA,
    SKIP,
  };
  ReadState read_state = ReadState::SIZE;

  std::size_t frame_bytes_received = 0;
  std::size_t frame_bytes_expected = 0;

  while (!m_read_shutdown.IsSet())
  {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(m_fd, &rfds);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;
    int select_res = select(m_fd + 1, &rfds, nullptr, nullptr, &timeout);
    if (select_res < 0)
    {
      ERROR_LOG(SP1, "Can\'t poll tapserver fd: %s", Common::StrNetworkError());
      continue;
    }
    if (select_res == 0)
      continue;

    // The tapserver protocol is very simple: there is a 16-bit little-endian
    // size field, followed by that many bytes of packet data
    switch (read_state)
    {
    case ReadState::SIZE:
    {
      u8 size_bytes[2];
      const ws_ssize_t bytes_read = recv(m_fd, reinterpret_cast<char*>(size_bytes), 2, 0);
      if (bytes_read == 1)
      {
        read_state = ReadState::SIZE_HIGH;
        frame_bytes_expected = size_bytes[0];
      }
      else if (bytes_read == 2)
      {
        frame_bytes_expected = size_bytes[0] | (size_bytes[1] << 8);
        if (frame_bytes_expected > m_max_frame_size)
        {
          ERROR_LOG(SP1, "Packet is too large (%" PRIu64 " bytes); dropping it",
                    (u64)frame_bytes_expected);
          read_state = ReadState::SKIP;
        }
        else
        {
          // If read is disabled, we still need to actually read the frame in
          // order to avoid applying backpressure on the remote end, but we
          // should drop the frame instead of forwarding it to the client.
          read_state = m_read_enabled.IsSet() ? ReadState::DATA : ReadState::SKIP;
        }
      }
      else
      {
        ERROR_LOG(SP1, "Failed to read size field from destination: %s", Common::StrNetworkError());
      }
      break;
    }
    case ReadState::SIZE_HIGH:
    {
      // This handles the annoying case where only one byte of the size field
      // was available earlier.
      u8 size_high = 0;
      const ws_ssize_t bytes_read = recv(m_fd, reinterpret_cast<char*>(&size_high), 1, 0);
      if (bytes_read != 1)
      {
        ERROR_LOG(SP1, "Failed to read split size field from destination: %s",
                  Common::StrNetworkError());
        break;
      }
      frame_bytes_expected |= (size_high << 8);
      if (frame_bytes_expected > m_max_frame_size)
      {
        ERROR_LOG(SP1, "Packet is too large (%" PRIu64 " bytes); dropping it",
                  (u64)frame_bytes_expected);
        read_state = ReadState::SKIP;
      }
      else
      {
        read_state = m_read_enabled.IsSet() ? ReadState::DATA : ReadState::SKIP;
      }
      break;
    }
    case ReadState::DATA:
    case ReadState::SKIP:
    {
      // A frame being kept lands whole in the frame buffer; one being skipped is read through
      // it a buffer at a time.
      std::size_t bytes_to_read = frame_bytes_expected - frame_bytes_received;
      u8* dest = m_frame_buffer + frame_bytes_received;
      if (read_state == ReadState::SKIP)
      {
        dest = m_frame_buffer;
        if (bytes_to_read > m_max_frame_size)
          bytes_to_read = m_max_frame_size;
      }
      const ws_ssize_t bytes_read =
          recv(m_fd, reinterpret_cast<char*>(dest), static_cast<ws_ssize_t>(bytes_to_read), 0);
      if (bytes_read <= 0)
      {
        ERROR_LOG(SP1, "Failed to read data from destination: %s", Common::StrNetworkError());
        break;
      }
      frame_bytes_received += bytes_read;
      if (frame_bytes_received == frame_bytes_expected)
      {
        if (read_state == ReadState::DATA)
        {
          m_recv_cb(m_recv_user, m_frame_buffer, frame_bytes_expected);
        }
        frame_bytes_received = 0;
        frame_bytes_expected = 0;
        read_state = ReadState::SIZE;
      }
      break;
    }
    }
  }
}

}  // namespace ExpansionInterface
