// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// No Wii socket support while using NetPlay or TAS
#include "Core/IOS/Network/Socket.h"

#include <algorithm>
#include <cstdlib>
#include <numeric>

#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#endif
#ifdef __HAIKU__
#include <sys/select.h>
#endif

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/NetworkCaptureLogger.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

#ifdef _WIN32
#define ERRORCODE(name) WSA##name
#define EITHER(win32, posix) win32
#else
#define ERRORCODE(name) name
#define EITHER(win32, posix) posix
#define closesocket close
#endif

namespace IOS::HLE
{
// The following functions can return
//  - EAGAIN / EWOULDBLOCK: send(to), recv(from), accept
//  - EINPROGRESS: connect, bind
//  - WSAEWOULDBLOCK: send(to), recv(from), accept, connect
// On Windows is_rw is used to correct the return value for connect
static s32 TranslateErrorCode(s32 native_error, bool is_rw)
{
  switch (native_error)
  {
  case ERRORCODE(EMSGSIZE):
    ERROR_LOG(IOS_NET, "Find out why this happened, looks like PEEK failure?");
    return -1;  // Should be -SO_EMSGSIZE
  case EITHER(WSAENOTSOCK, EBADF):
    return -SO_EBADF;
  case ERRORCODE(EADDRINUSE):
    return -SO_EADDRINUSE;
  case ERRORCODE(ECONNRESET):
    return -SO_ECONNRESET;
  case ERRORCODE(EISCONN):
    return -SO_EISCONN;
  case ERRORCODE(ENOTCONN):
    return -SO_ENOTCONN;
  case ERRORCODE(EINPROGRESS):
    return -SO_EINPROGRESS;
  case ERRORCODE(EALREADY):
    return -SO_EALREADY;
  case ERRORCODE(EACCES):
    return -SO_EACCES;
  case ERRORCODE(ECONNREFUSED):
    return -SO_ECONNREFUSED;
  case ERRORCODE(ENETUNREACH):
    return -SO_ENETUNREACH;
  case ERRORCODE(EHOSTUNREACH):
    return -SO_EHOSTUNREACH;
  case ENOMEM:  // See man (7) ip
  case ERRORCODE(ENOBUFS):
    return -SO_ENOMEM;
  case ERRORCODE(ENETRESET):
    return -SO_ENETRESET;
  case EITHER(WSAEWOULDBLOCK, EAGAIN):
    return (is_rw) ? (-SO_EAGAIN) : (-SO_EINPROGRESS);
  default:
    return -1;
  }
}

WiiSockMan::WiiSockMan(EmulationKernel& ios) : m_ios(ios)
{
}

WiiSockMan::~WiiSockMan()
{
  Clean();
}

WiiSocket* WiiSockMan::FindSocket(s32 wii_fd)
{
  if (wii_fd < 0 || wii_fd >= WII_SOCKET_FD_MAX || !m_socket_used[wii_fd])
    return nullptr;
  return reinterpret_cast<WiiSocket*>(m_socket_storage[wii_fd]);
}

const WiiSocket* WiiSockMan::FindSocket(s32 wii_fd) const
{
  if (wii_fd < 0 || wii_fd >= WII_SOCKET_FD_MAX || !m_socket_used[wii_fd])
    return nullptr;
  return reinterpret_cast<const WiiSocket*>(m_socket_storage[wii_fd]);
}

void WiiSockMan::DestroySocket(s32 wii_fd)
{
  WiiSocket* const socket = FindSocket(wii_fd);
  if (!socket)
    return;
  socket->~WiiSocket();
  m_socket_used[wii_fd] = false;
}

void WiiSockMan::Clean()
{
  for (s32 wii_fd = 0; wii_fd < WII_SOCKET_FD_MAX; wii_fd++)
    DestroySocket(wii_fd);
}

// Don't use string! (see https://github.com/dolphin-emu/dolphin/pull/3143)
s32 WiiSockMan::GetNetErrorCode(s32 ret, const char* caller, bool is_rw)
{
#ifdef _WIN32
  s32 error_code = WSAGetLastError();
  // Some programs might hijack WinSock2 (e.g. ReShade) and alter the expected return value.
  if (error_code == WSAEINVAL && std::strcmp(caller, "SO_CONNECT") == 0)
  {
    // Note:
    // In order to preserve backward compatibility, this error is reported as WSAEINVAL to Windows
    // Sockets 1.1 applications that link to either Winsock.dll or Wsock32.dll.
    //
    // Source:
    // https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-connect
    error_code = WSAEALREADY;
  }
#else
  s32 error_code = errno;
#endif

  if (ret >= 0)
  {
    SetLastNetError(ret);
    return ret;
  }

  const s32 return_value = TranslateErrorCode(error_code, is_rw);
  const auto level =
      return_value == -SO_EAGAIN ? Common::Log::LogLevel::LINFO : Common::Log::LogLevel::LERROR;
  GENERIC_LOG(Common::Log::LogType::IOS_NET, level, "%s failed with error %d: %s, ret = %d", caller,
              error_code, Common::DecodeNetworkError(error_code), ret);

  SetLastNetError(return_value);

  return return_value;
}

WiiSocket::~WiiSocket()
{
  if (fd >= 0)
  {
    (void)CloseFd();
  }
  while (pending_sockop_count != 0)
    RemoveSockop(pending_sockop_count - 1);
}

void WiiSocket::SetFd(s32 s)
{
  if (fd >= 0)
    (void)CloseFd();

  nonBlock = false;
  fd = s;

// Set socket to NON-BLOCK
#ifdef _WIN32
  u_long iMode = 1;
  ioctlsocket(fd, FIONBIO, &iMode);
#else
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
    flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void WiiSocket::SetWiiFd(s32 s)
{
  wii_fd = s;
}

s32 WiiSocket::Shutdown(u32 how)
{
  if (how > 2)
    return -SO_EINVAL;

  // The Wii does nothing and returns 0 for IP_PROTO_UDP
  int so_type;
  socklen_t opt_len = sizeof(so_type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&so_type), &opt_len) != 0 ||
      (so_type != SOCK_STREAM && so_type != SOCK_DGRAM))
    return -SO_EBADF;
  if (so_type == SOCK_DGRAM)
    return SO_SUCCESS;

  // Adjust pending operations
  // Values based on https://dolp.in/pr8758 hwtest
  const s32 ret = m_socket_manager.GetNetErrorCode(shutdown(fd, how), "SO_SHUTDOWN", false);
  const bool shut_read = how == 0 || how == 2;
  const bool shut_write = how == 1 || how == 2;
  for (u32 i = 0; i < pending_sockop_count; i++)
  {
    sockop& op = *GetSockop(i);
    // TODO: Create hwtest for SSL
    if (op.is_ssl)
      continue;

    switch (op.net_type)
    {
    case IOCTL_SO_ACCEPT:
      if (shut_write)
        Abort(&op, -SO_EINVAL);
      break;
    case IOCTL_SO_CONNECT:
      if (shut_write && !nonBlock)
        Abort(&op, -SO_ENETUNREACH);
      break;
    case IOCTLV_SO_RECVFROM:
      if (shut_read)
        Abort(&op, -SO_ENOTCONN);
      break;
    case IOCTLV_SO_SENDTO:
      if (shut_write)
        Abort(&op, -SO_ENOTCONN);
      break;
    default:
      break;
    }
  }
  return ret;
}

s32 WiiSocket::CloseFd()
{
  s32 ReturnValue = 0;
  if (fd >= 0)
  {
    s32 ret = closesocket(fd);
    ReturnValue = m_socket_manager.GetNetErrorCode(ret, "CloseFd", false);
  }
  else
  {
    ReturnValue = m_socket_manager.GetNetErrorCode(EITHER(WSAENOTSOCK, EBADF), "CloseFd", false);
  }
  fd = -1;

  while (pending_sockop_count != 0)
  {
    m_socket_manager.EnqueueIPCReply(GetSockop(0)->request, -SO_ENOTCONN);
    RemoveSockop(0);
  }
  connecting_state = ConnectingState::None;
  return ReturnValue;
}

s32 WiiSocket::FCntl(u32 cmd, u32 arg)
{
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#define F_NONBLOCK 4
  s32 ret = 0;
  if (cmd == F_GETFL)
  {
    ret = nonBlock ? F_NONBLOCK : 0;
  }
  else if (cmd == F_SETFL)
  {
    nonBlock = (arg & F_NONBLOCK) == F_NONBLOCK;
  }
  else
  {
    ERROR_LOG(IOS_NET, "SO_FCNTL unknown command");
  }

  INFO_LOG(IOS_NET, "IOCTL_SO_FCNTL(%08x, %08X, %08X)", (unsigned)wii_fd, cmd, arg);

  return ret;
}

void WiiSocket::Update(bool read, bool write, bool except)
{
  auto& system = m_socket_manager.m_ios.GetSystem();
  auto& memory = system.GetMemory();

  u32 index = 0;
  while (index < pending_sockop_count)
  {
    sockop* const it = GetSockop(index);
    s32 ReturnValue = 0;
    bool forceNonBlock = false;
    IPCCommandType ct = it->request.command;
    if (!it->is_ssl && ct == IPC_CMD_IOCTL)
    {
      IOCtlRequest ioctl{system, it->request.address};
      switch (it->net_type)
      {
      case IOCTL_SO_FCNTL:
      {
        u32 cmd = memory.Read_U32(ioctl.buffer_in + 4);
        u32 arg = memory.Read_U32(ioctl.buffer_in + 8);
        ReturnValue = FCntl(cmd, arg);
        break;
      }
      case IOCTL_SO_BIND:
      {
        WiiSockAddrIn addr;
        memory.CopyFromEmu(&addr, ioctl.buffer_in + 8, sizeof(WiiSockAddrIn));
        sockaddr_in local_name = WiiSockMan::ToNativeAddrIn(addr);

        int ret = bind(fd, (sockaddr*)&local_name, sizeof(local_name));
        ReturnValue = m_socket_manager.GetNetErrorCode(ret, "SO_BIND", false);

        INFO_LOG(IOS_NET, "IOCTL_SO_BIND (%08X, %s:%u) = %d", (unsigned)wii_fd,
                 inet_ntoa(local_name.sin_addr), (unsigned)Common::swap16(local_name.sin_port),
                 ret);
        break;
      }
      case IOCTL_SO_CONNECT:
      {
        WiiSockAddrIn addr;
        memory.CopyFromEmu(&addr, ioctl.buffer_in + 8, sizeof(WiiSockAddrIn));
        sockaddr_in local_name = WiiSockMan::ToNativeAddrIn(addr);

        const int ret = connect(fd, (sockaddr*)&local_name, sizeof(local_name));
        ReturnValue = m_socket_manager.GetNetErrorCode(ret, "SO_CONNECT", false);
        UpdateConnectingState(ReturnValue);

        INFO_LOG(IOS_NET, "IOCTL_SO_CONNECT (%08x, %s:%u) = %d", (unsigned)wii_fd,
                 inet_ntoa(local_name.sin_addr), (unsigned)Common::swap16(local_name.sin_port),
                 ret);
        break;
      }
      case IOCTL_SO_ACCEPT:
      {
        s32 ret;
        if (ioctl.buffer_out_size > 0)
        {
          WiiSockAddrIn addr;
          memory.CopyFromEmu(&addr, ioctl.buffer_out, sizeof(WiiSockAddrIn));
          sockaddr_in local_name = WiiSockMan::ToNativeAddrIn(addr);

          socklen_t addrlen = sizeof(sockaddr_in);
          ret = static_cast<s32>(accept(fd, (sockaddr*)&local_name, &addrlen));

          WiiSockAddrIn new_addr = WiiSockMan::ToWiiAddrIn(local_name, addrlen);
          memory.CopyToEmu(ioctl.buffer_out, &new_addr, sizeof(WiiSockAddrIn));
        }
        else
        {
          ret = static_cast<s32>(accept(fd, nullptr, nullptr));
        }

        ReturnValue = m_socket_manager.AddSocket(ret, true);

        ioctl.Log("IOCTL_SO_ACCEPT", Common::Log::LogType::IOS_NET);
        break;
      }
      default:
        break;
      }

      // Fix blocking error codes
      if (!nonBlock && it->net_type == IOCTL_SO_CONNECT)
      {
        switch (ReturnValue)
        {
        case -SO_EAGAIN:
        case -SO_EALREADY:
        case -SO_EINPROGRESS:
          if (Clock::now() > GetTimeout())
          {
            ReturnValue = -SO_ENETUNREACH;
            ResetTimeout();
            connecting_state = ConnectingState::Error;
          }
          break;
        case -SO_EISCONN:
          ReturnValue = SO_SUCCESS;
          connecting_state = ConnectingState::Connected;
          [[fallthrough]];
        default:
          ResetTimeout();
        }
      }
    }
    else if (ct == IPC_CMD_IOCTLV)
    {
      IOCtlVRequest ioctlv{system, it->request.address};
      u32 BufferIn = 0, BufferIn2 = 0;
      u32 BufferInSize = 0, BufferInSize2 = 0;
      u32 BufferOut = 0, BufferOut2 = 0;
      u32 BufferOutSize = 0, BufferOutSize2 = 0;

      if (!ioctlv.in_vectors.empty())
      {
        BufferIn = ioctlv.in_vectors.at(0).address;
        BufferInSize = ioctlv.in_vectors.at(0).size;
      }

      if (!ioctlv.io_vectors.empty())
      {
        BufferOut = ioctlv.io_vectors.at(0).address;
        BufferOutSize = ioctlv.io_vectors.at(0).size;
      }

      if (ioctlv.io_vectors.size() > 1)
      {
        BufferOut2 = ioctlv.io_vectors.at(1).address;
        BufferOutSize2 = ioctlv.io_vectors.at(1).size;
      }

      if (ioctlv.in_vectors.size() > 1)
      {
        BufferIn2 = ioctlv.in_vectors.at(1).address;
        BufferInSize2 = ioctlv.in_vectors.at(1).size;
      }

      if (it->is_ssl)
      {
        int sslID = memory.Read_U32(BufferOut) - 1;
        if (IsSSLIDValid(sslID))
        {
          switch (it->ssl_type)
          {
          case IOCTLV_NET_SSL_DOHANDSHAKE:
          {
            // The Wii allows a socket with an in-progress connection to
            // perform the SSL handshake. The TLS client does not support it so
            // we have to check it manually.
            connecting_state = GetConnectingState();
            if (connecting_state == ConnectingState::Connecting)
            {
              WriteReturnValue(memory, SSL_ERR_RAGAIN, BufferIn);
              ReturnValue = SSL_ERR_RAGAIN;
              break;
            }
            else if (connecting_state == ConnectingState::None ||
                     connecting_state == ConnectingState::Error)
            {
              WriteReturnValue(memory, SSL_ERR_SYSCALL, BufferIn);
              ReturnValue = SSL_ERR_SYSCALL;
              break;
            }

            const s32 ret = WiiSSLHandshakeStep(&NetSSLDevice::_SSL[sslID]);
            WriteReturnValue(memory, ret, BufferIn);
            if (!nonBlock && ret != SSL_OK && ret != SSL_ERR_FAILED)
              ReturnValue = ret;

            INFO_LOG(IOS_SSL,
                     "IOCTLV_NET_SSL_DOHANDSHAKE = (%d) "
                     "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
                     "BufferOut: (%08x, %u), BufferOut2: (%08x, %u)",
                     ret, BufferIn, BufferInSize, BufferIn2, BufferInSize2, BufferOut,
                     BufferOutSize, BufferOut2, BufferOutSize2);
            break;
          }
          case IOCTLV_NET_SSL_WRITE:
          {
            WII_SSL* ssl = &NetSSLDevice::_SSL[sslID];
            const u8* const data = memory.GetPointerForRange(BufferOut2, BufferOutSize2);
            const s32 ret = data ? WiiSSLWrite(ssl, data, BufferOutSize2) : SSL_ERR_FAILED;
            if (ret > 0)
              Core::NetworkLogger()->LogSSLWrite(data, ret, ssl->hostfd);
            WriteReturnValue(memory, ret, BufferIn);
            if (!nonBlock && (ret == SSL_ERR_RAGAIN || ret == SSL_ERR_WAGAIN))
              ReturnValue = ret;
            break;
          }
          case IOCTLV_NET_SSL_READ:
          {
            WII_SSL* ssl = &NetSSLDevice::_SSL[sslID];
            u8* const data = memory.GetPointerForRange(BufferIn2, BufferInSize2);
            const s32 ret = data ? WiiSSLRead(ssl, data, BufferInSize2) : SSL_ERR_FAILED;
            if (ret > 0)
              Core::NetworkLogger()->LogSSLRead(data, ret, ssl->hostfd);
            WriteReturnValue(memory, ret, BufferIn);
            if (!nonBlock && (ret == SSL_ERR_RAGAIN || ret == SSL_ERR_WAGAIN))
              ReturnValue = ret;
            break;
          }
          default:
            break;
          }
        }
        else
        {
          WriteReturnValue(memory, SSL_ERR_ID, BufferIn);
        }
      }
      else
      {
        switch (it->net_type)
        {
        case IOCTLV_SO_SENDTO:
        {
          // The Wii allows a socket with a connection in progress to use
          // sendto(). This might not be supported by the operating system.
          // We have to enforce it manually.
          connecting_state = GetConnectingState();
          if (nonBlock && IsTCP() && connecting_state == ConnectingState::Connecting)
          {
            ReturnValue = -SO_EAGAIN;
            break;
          }

          u32 flags = memory.Read_U32(BufferIn2 + 0x04);
          u32 has_destaddr = memory.Read_U32(BufferIn2 + 0x08);

          // Not a string, Windows requires a const char* for sendto
          const char* data = (const char*)memory.GetPointerForRange(BufferIn, BufferInSize);
          if (!data && BufferInSize != 0)
          {
            // what a send from a bad buffer fails with: nothing is read from it
            ReturnValue = -SO_EFAULT;
            break;
          }
          size_t patched_size = 0;
          char* const patched =
              WC24PatchEngine::GetNetworkPatchByPayload(data, BufferInSize, &patched_size);
          if (patched)
          {
            data = patched;
            BufferInSize = static_cast<u32>(patched_size);
          }

          // Act as non blocking when SO_MSG_NONBLOCK is specified
          forceNonBlock = ((flags & SO_MSG_NONBLOCK) == SO_MSG_NONBLOCK);
          // send/sendto only handles MSG_OOB
          flags &= SO_MSG_OOB;

          sockaddr_in local_name = {0};
          if (has_destaddr)
          {
            WiiSockAddrIn addr;
            memory.CopyFromEmu(&addr, BufferIn2 + 0x0C, sizeof(WiiSockAddrIn));
            local_name = WiiSockMan::ToNativeAddrIn(addr);
          }

          auto* to = has_destaddr ? reinterpret_cast<sockaddr*>(&local_name) : nullptr;
          socklen_t tolen = has_destaddr ? sizeof(sockaddr) : 0;
          const int ret = sendto(fd, data, BufferInSize, flags, to, tolen);
          ReturnValue = m_socket_manager.GetNetErrorCode(ret, "SO_SENDTO", true);
          if (ret > 0)
            Core::NetworkLogger()->LogWrite(data, ret, fd, to);

          INFO_LOG(IOS_NET,
                   "%s = %d Socket: %08x, BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
                   "%u.%u.%u.%u",
                   has_destaddr ? "IOCTLV_SO_SENDTO " : "IOCTLV_SO_SEND ", ReturnValue,
                   (unsigned)wii_fd, BufferIn, BufferInSize, BufferIn2, BufferInSize2,
                   static_cast<unsigned>(local_name.sin_addr.s_addr & 0xFF),
                   static_cast<unsigned>((local_name.sin_addr.s_addr >> 8) & 0xFF),
                   static_cast<unsigned>((local_name.sin_addr.s_addr >> 16) & 0xFF),
                   static_cast<unsigned>((local_name.sin_addr.s_addr >> 24) & 0xFF));
          std::free(patched);
          break;
        }
        case IOCTLV_SO_RECVFROM:
        {
          // The Wii allows a socket with a connection in progress to use
          // recvfrom(). This might not be supported by the operating system.
          // We have to enforce it manually.
          connecting_state = GetConnectingState();
          if (nonBlock && IsTCP() && connecting_state == ConnectingState::Connecting)
          {
            ReturnValue = -SO_EAGAIN;
            break;
          }

          u32 flags = memory.Read_U32(BufferIn + 0x04);
          int data_len = BufferOutSize;
          // Not a string, Windows requires a char* for recvfrom
          char* data = reinterpret_cast<char*>(memory.GetPointerForRange(BufferOut, BufferOutSize));

          sockaddr_in local_name;
          memset(&local_name, 0, sizeof(sockaddr_in));

          if (BufferOutSize2 != 0)
          {
            WiiSockAddrIn addr;
            memory.CopyFromEmu(&addr, BufferOut2, sizeof(WiiSockAddrIn));
            local_name = WiiSockMan::ToNativeAddrIn(addr);
          }

          // Act as non blocking when SO_MSG_NONBLOCK is specified
          forceNonBlock = ((flags & SO_MSG_NONBLOCK) == SO_MSG_NONBLOCK);

          // recv/recvfrom only handles PEEK/OOB
          flags &= SO_MSG_PEEK | SO_MSG_OOB;
#ifdef _WIN32
          if (flags & SO_MSG_PEEK)
          {
            unsigned long totallen = 0;
            ioctlsocket(fd, FIONREAD, &totallen);
            ReturnValue = totallen;
            break;
          }
#endif
          socklen_t addrlen = sizeof(sockaddr_in);
          auto* from = BufferOutSize2 ? reinterpret_cast<sockaddr*>(&local_name) : nullptr;
          socklen_t* fromlen = BufferOutSize2 ? &addrlen : nullptr;
          const int ret = recvfrom(fd, data, data_len, flags, from, fromlen);
          ReturnValue = m_socket_manager.GetNetErrorCode(
              ret, BufferOutSize2 ? "SO_RECVFROM" : "SO_RECV", true);
          if (ret > 0)
            Core::NetworkLogger()->LogRead(data, ret, fd, from);

          INFO_LOG(IOS_NET,
                   "%s(%d, %p) Socket: %08X, Flags: %08X, "
                   "BufferIn: (%08x, %u), BufferIn2: (%08x, %u), "
                   "BufferOut: (%08x, %u), BufferOut2: (%08x, %u)",
                   BufferOutSize2 ? "IOCTLV_SO_RECVFROM " : "IOCTLV_SO_RECV ", ReturnValue,
                   static_cast<const void*>(data), (unsigned)wii_fd, flags, BufferIn, BufferInSize,
                   BufferIn2, BufferInSize2, BufferOut, BufferOutSize, BufferOut2, BufferOutSize2);

          if (BufferOutSize2 != 0)
          {
            WiiSockAddrIn new_addr = WiiSockMan::ToWiiAddrIn(local_name, addrlen);
            memory.CopyToEmu(BufferOut2, &new_addr, sizeof(WiiSockAddrIn));
          }
          break;
        }
        default:
          break;
        }
      }
    }

    if (it->is_aborted)
    {
      RemoveSockop(index);
      continue;
    }

    if (nonBlock || forceNonBlock ||
        (!it->is_ssl && ReturnValue != -SO_EAGAIN && ReturnValue != -SO_EINPROGRESS &&
         ReturnValue != -SO_EALREADY) ||
        (it->is_ssl && ReturnValue != SSL_ERR_WAGAIN && ReturnValue != SSL_ERR_RAGAIN))
    {
      DEBUG_LOG(IOS_NET,
                "IOCTL(V) Sock: %08x ioctl/v: %d returned: %d nonBlock: %s forceNonBlock: %s",
                (unsigned)wii_fd,
                it->is_ssl ? static_cast<int>(it->ssl_type) : static_cast<int>(it->net_type),
                ReturnValue, nonBlock ? "true" : "false", forceNonBlock ? "true" : "false");

      m_socket_manager.EnqueueIPCReply(it->request, ReturnValue);
      RemoveSockop(index);
    }
    else
    {
      ++index;
    }
  }
}

void WiiSocket::UpdateConnectingState(s32 connect_rv)
{
  if (connect_rv == -SO_EAGAIN || connect_rv == -SO_EALREADY || connect_rv == -SO_EINPROGRESS)
  {
    connecting_state = ConnectingState::Connecting;
  }
  else if (connect_rv >= 0)
  {
    connecting_state = ConnectingState::Connected;
  }
  else
  {
    connecting_state = ConnectingState::Error;
  }
}

WiiSocket::ConnectingState WiiSocket::GetConnectingState() const
{
  const auto state = Common::SaveNetworkErrorState();
  Common::ScopeGuard guard([&state] { Common::RestoreNetworkErrorState(state); });

#ifdef _WIN32
  constexpr int (*get_errno)() = &WSAGetLastError;
#else
  constexpr int (*get_errno)() = []() { return errno; };
#endif

  switch (connecting_state)
  {
  case ConnectingState::Error:
  case ConnectingState::Connected:
  case ConnectingState::None:
    break;
  case ConnectingState::Connecting:
  {
    const s32 nfds = fd + 1;
    fd_set read_fds;
    fd_set write_fds;
    fd_set except_fds;
    timeval t = {0, 0};
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);
    FD_SET(fd, &write_fds);
    FD_SET(fd, &except_fds);

    if (select(nfds, &read_fds, &write_fds, &except_fds, &t) < 0)
    {
      const s32 error = get_errno();
      ERROR_LOG(IOS_SSL, "Failed to get socket (fd=%d) connection state (err=%d): %s", wii_fd,
                error, Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    if (FD_ISSET(fd, &write_fds) == 0 && FD_ISSET(fd, &except_fds) == 0)
      break;

    s32 error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) != 0)
    {
      error = get_errno();
      ERROR_LOG(IOS_SSL, "Failed to get socket (fd=%d) error state (err=%d): %s", wii_fd, error,
                Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    if (error != 0)
    {
      ERROR_LOG(IOS_SSL, "Non-blocking connect (fd=%d) failed (err=%d): %s", wii_fd, error,
                Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    // Get peername to ensure the socket is connected
    sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0)
    {
      error = get_errno();
      ERROR_LOG(IOS_SSL, "Non-blocking connect (fd=%d) failed to get peername (err=%d): %s", wii_fd,
                error, Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    INFO_LOG(IOS_SSL, "Non-blocking connect (fd=%d) succeeded", wii_fd);
    return ConnectingState::Connected;
  }
  }

  return connecting_state;
}

bool WiiSocket::IsTCP() const
{
  const auto state = Common::SaveNetworkErrorState();
  Common::ScopeGuard guard([&state] { Common::RestoreNetworkErrorState(state); });

  int socket_type;
  socklen_t option_length = sizeof(socket_type);
  return getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socket_type),
                    &option_length) == 0 &&
         socket_type == SOCK_STREAM;
}

const WiiSocket::Timeout& WiiSocket::GetTimeout()
{
  if (!has_timeout)
  {
    timeout = Clock::now() + DTFromSeconds(Config::Get(Config::MAIN_NETWORK_TIMEOUT));
    has_timeout = true;
  }
  return timeout;
}

void WiiSocket::ResetTimeout()
{
  has_timeout = false;
}

void WiiSocket::DoSock(const Request& request, NET_IOCTL type)
{
  sockop so = {request, false};
  so.net_type = type;
  if (!AddSockop(so))
    m_socket_manager.EnqueueIPCReply(request, -SO_ENOBUFS);
}

void WiiSocket::DoSock(const Request& request, SSL_IOCTL type)
{
  sockop so = {request, true};
  so.ssl_type = type;
  if (!AddSockop(so))
    m_socket_manager.EnqueueIPCReply(request, SSL_ERR_FAILED);
}

WiiSocket::sockop* WiiSocket::GetSockop(u32 index)
{
  return reinterpret_cast<sockop*>(pending_sockop_storage[index]);
}

bool WiiSocket::AddSockop(const sockop& op)
{
  if (pending_sockop_count == MAX_PENDING_SOCKOPS)
  {
    ERROR_LOG(IOS_NET, "Socket %d: too many pending operations", static_cast<int>(wii_fd));
    return false;
  }
  new (pending_sockop_storage[pending_sockop_count]) sockop(op);
  pending_sockop_count++;
  return true;
}

void WiiSocket::RemoveSockop(u32 index)
{
  for (u32 i = index; i + 1 < pending_sockop_count; i++)
    *GetSockop(i) = *GetSockop(i + 1);
  pending_sockop_count--;
  GetSockop(pending_sockop_count)->~sockop();
}

s32 WiiSockMan::AddSocket(s32 fd, bool is_rw)
{
  const char* caller = is_rw ? "SO_ACCEPT" : "NewSocket";

  if (fd < 0)
    return GetNetErrorCode(fd, caller, is_rw);

  s32 wii_fd;
  for (wii_fd = 0; wii_fd < WII_SOCKET_FD_MAX; ++wii_fd)
  {
    // Find an available socket fd
    if (!m_socket_used[wii_fd])
      break;
  }

  if (wii_fd == WII_SOCKET_FD_MAX)
  {
    // Close host socket
    closesocket(fd);
    wii_fd = -SO_EMFILE;
    ERROR_LOG(IOS_NET, "%s failed: Too many open sockets, ret=%d", caller, wii_fd);
  }
  else
  {
    WiiSocket& sock = *new (m_socket_storage[wii_fd]) WiiSocket(*this);
    m_socket_used[wii_fd] = true;
    sock.SetFd(fd);
    sock.SetWiiFd(wii_fd);
    Core::NetworkLogger()->OnNewSocket(fd);

    Common::SetPlatformSocketOptions(fd);

    // Wii UDP sockets can use broadcast address by default
    if (!is_rw)
    {
      const auto state = Common::SaveNetworkErrorState();
      Common::ScopeGuard guard([&state] { Common::RestoreNetworkErrorState(state); });

      int socket_type;
      socklen_t option_length = sizeof(socket_type);
      const bool is_udp = getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socket_type),
                                     &option_length) == 0 &&
                          socket_type == SOCK_DGRAM;
      const int opt_broadcast = 1;
      if (is_udp &&
          setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt_broadcast),
                     sizeof(opt_broadcast)) != 0)
      {
        ERROR_LOG(IOS_NET, "Failed to set SO_BROADCAST on socket");
      }
    }
  }

  SetLastNetError(wii_fd);
  return wii_fd;
}

s32 WiiSockMan::NewSocket(s32 af, s32 type, s32 protocol)
{
  if (af == 2)
  {
    // AF_INET == 2 is true on all systems I've seen,
    // but it's not guaranteed. Better set this again.
    af = AF_INET;
  }
  else if (af == 23)
  {
    // AF_INET6 == 23 is only true on Wii and on Windows.
    // On other OSes, AF_INET6 can have a different value.
    // For example, on Linux it's 10 and on MacOS/iOS it's 30.
    af = AF_INET6;
  }
  else
  {
    // Neither an AF_INET nor an AF_INET6 socket.
    // Unsupported.
    return -SO_EAFNOSUPPORT;
  }

  if (protocol != 0)  // IPPROTO_IP
    return -SO_EPROTONOSUPPORT;
  if (type != 1 && type != 2)  // SOCK_STREAM && SOCK_DGRAM
    return -SO_EPROTOTYPE;

  s32 fd = static_cast<s32>(socket(af, type, protocol));
  return AddSocket(fd, false);
}

s32 WiiSockMan::GetHostSocket(s32 wii_fd) const
{
  const WiiSocket* const socket = FindSocket(wii_fd);
  if (socket)
    return socket->fd;
  return -EBADF;
}

s32 WiiSockMan::ShutdownSocket(s32 wii_fd, u32 how)
{
  WiiSocket* const socket = FindSocket(wii_fd);
  if (socket)
    return socket->Shutdown(how);
  return -SO_EBADF;
}

s32 WiiSockMan::DeleteSocket(s32 wii_fd)
{
  s32 ReturnValue = -SO_EBADF;
  WiiSocket* const socket = FindSocket(wii_fd);
  if (socket)
  {
    ReturnValue = socket->CloseFd();
    DestroySocket(wii_fd);
  }
  return ReturnValue;
}

void WiiSockMan::EnqueueIPCReply(const Request& request, s32 return_value) const
{
  m_ios.EnqueueIPCReply(request, return_value);
}

void WiiSockMan::Update()
{
  s32 nfds = 0;
  fd_set read_fds, write_fds, except_fds;
  timeval t = {0, 0};
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);
  FD_ZERO(&except_fds);

  for (s32 wii_fd = 0; wii_fd < WII_SOCKET_FD_MAX; wii_fd++)
  {
    const WiiSocket* const sock = FindSocket(wii_fd);
    if (!sock)
      continue;
    if (sock->IsValid())
    {
      FD_SET(sock->fd, &read_fds);
      FD_SET(sock->fd, &write_fds);
      FD_SET(sock->fd, &except_fds);
      if (sock->fd + 1 > nfds)
        nfds = sock->fd + 1;
    }
    else
    {
      // Good time to clean up invalid sockets.
      DestroySocket(wii_fd);
    }
  }

  const s32 ret = select(nfds, &read_fds, &write_fds, &except_fds, &t);

  // A socket an update accepts may land in a later slot and be updated in this pass too, with
  // nothing set for it.
  for (s32 wii_fd = 0; wii_fd < WII_SOCKET_FD_MAX; wii_fd++)
  {
    WiiSocket* const sock = FindSocket(wii_fd);
    if (!sock)
      continue;
    if (ret >= 0 && sock->IsValid())
    {
      sock->Update(FD_ISSET(sock->fd, &read_fds) != 0, FD_ISSET(sock->fd, &write_fds) != 0,
                   FD_ISSET(sock->fd, &except_fds) != 0);
    }
    else
    {
      sock->Update(false, false, false);
    }
  }
  UpdatePollCommands();
}

void WiiSockMan::UpdatePollCommands()
{
  static constexpr int error_event = (POLLHUP | POLLERR);

  if (pending_poll_count == 0)
    return;

  const auto now = Clock::now();
  const s64 elapsed = DTToMs(now - last_time);
  last_time = now;

  for (u32 i = 0; i < pending_poll_count; i++)
  {
    PollCommand& pcmd = pending_polls[i];
    // Don't touch negative timeouts
    if (pcmd.timeout > 0)
      pcmd.timeout = pcmd.timeout > elapsed ? pcmd.timeout - elapsed : 0;
  }

  auto& system = m_ios.GetSystem();
  auto& memory = system.GetMemory();

  // Commands that finish are replied to and removed; the rest keep their order.
  u32 kept = 0;
  for (u32 command = 0; command < pending_poll_count; command++)
  {
    PollCommand& pcmd = pending_polls[command];
    const auto request = Request(system, pcmd.request_addr);
    pollfd_t* const pfds = pcmd.wii_fds;
    const u32 pfd_count = pcmd.wii_fd_count;
    int ret = 0;

    // Happens only on savestate load
    if (pfds[0].revents & error_event)
    {
      ret = static_cast<int>(pfd_count);
    }
    else
    {
      // Make the behavior of poll consistent across platforms by not passing:
      //  - Set with invalid fds, revents is set to 0 (Linux) or POLLNVAL (Windows)
      //  - Set without a valid socket, raises an error on Windows
      // The pollfds with valid fds are polled together and put back where they were.
      pollfd_t valid[WII_SOCKET_FD_MAX];
      u32 valid_index[WII_SOCKET_FD_MAX];
      u32 n_valid = 0;
      for (u32 i = 0; i < pfd_count; ++i)
      {
        if (GetHostSocket(memory.Read_U32(pcmd.buffer_out + 0xc * i)) >= 0)
        {
          valid_index[n_valid] = i;
          valid[n_valid] = pfds[i];
          n_valid++;
        }
      }

      if (n_valid > 0)
        ret = poll(valid, n_valid, 0);
      if (ret < 0)
        ret = GetNetErrorCode(ret, "UpdatePollCommands", false);

      for (u32 i = 0; i < n_valid; ++i)
        pfds[valid_index[i]] = valid[i];
    }

    if (ret == 0 && pcmd.timeout)
    {
      if (kept != command)
        pending_polls[kept] = pcmd;
      kept++;
      continue;
    }

    // Translate native to Wii events,
    for (u32 i = 0; i < pfd_count; ++i)
    {
      const int revents = ConvertEvents(pfds[i].revents, ConvertDirection::NativeToWii);

      // No need to change fd or events as they are input only.
      // memory.Write_U32(ufds[i].fd, request.buffer_out + 0xc*i); //fd
      // memory.Write_U32(events, request.buffer_out + 0xc*i + 4); //events
      memory.Write_U32(revents, pcmd.buffer_out + 0xc * i + 8);  // revents
      DEBUG_LOG(IOS_NET, "IOCTL_SO_POLL socket %u wevents %08X events %08X revents %08X", i,
                (unsigned)revents, (int)pfds[i].events, (int)pfds[i].revents);
    }
    EnqueueIPCReply(request, ret);
  }
  pending_poll_count = kept;
}

sockaddr_in WiiSockMan::ToNativeAddrIn(WiiSockAddrIn from)
{
  sockaddr_in result;

  result.sin_addr.s_addr = from.addr.addr;
  result.sin_family = from.family;
  result.sin_port = from.port;

  return result;
}

s32 WiiSockMan::ConvertEvents(s32 events, ConvertDirection dir)
{
  constexpr struct
  {
    int native;
    int wii;
  } mapping[] = {
      {POLLRDNORM, 0x0001}, {POLLRDBAND, 0x0002}, {POLLPRI, 0x0004}, {POLLWRNORM, 0x0008},
      {POLLWRBAND, 0x0010}, {POLLERR, 0x0020},    {POLLHUP, 0x0040}, {POLLNVAL, 0x0080},
  };

  s32 converted_events = 0;
  s32 unhandled_events = 0;

  if (dir == ConvertDirection::NativeToWii)
  {
    for (const auto& map : mapping)
    {
      if (events & map.native)
        converted_events |= map.wii;
    }
  }
  else
  {
    unhandled_events = events;
    for (const auto& map : mapping)
    {
      if (events & map.wii)
        converted_events |= map.native;
      unhandled_events &= ~map.wii;
    }
  }
  if (unhandled_events)
    ERROR_LOG(IOS_NET, "SO_POLL: unhandled Wii event types: %04x", (unsigned)unhandled_events);
  return converted_events;
}

WiiSockAddrIn WiiSockMan::ToWiiAddrIn(const sockaddr_in& from, socklen_t addrlen)
{
  WiiSockAddrIn result;

  /* socklen_t is signed on Windows: a length the host gives as negative is none */
  const size_t length = addrlen > 0 ? static_cast<size_t>(addrlen) : 0;
  result.len = u8(length > sizeof(WiiSockAddrIn) ? sizeof(WiiSockAddrIn) : length);
  result.family = u8(from.sin_family & 0xFF);
  result.port = from.sin_port;
  result.addr.addr = from.sin_addr.s_addr;

  return result;
}

void WiiSockMan::DoState(PointerWrap& p)
{
  // Saved as the container was: a size_t count, then each command's addresses and its pollfds
  // as a u32 count and the structures.
  bool saving = p.IsWriteMode() || p.IsMeasureMode();
  size_t size = pending_poll_count;
  p.Do(size);
  if (!saving)
  {
    if (size > MAX_PENDING_POLLS)
    {
      p.SetMeasureMode();
      return;
    }
    pending_poll_count = static_cast<u32>(size);
  }
  for (u32 i = 0; i < pending_poll_count; i++)
  {
    PollCommand& pcmd = pending_polls[i];
    p.Do(pcmd.request_addr);
    p.Do(pcmd.buffer_out);
    u32 fd_count = pcmd.wii_fd_count;
    p.Do(fd_count);
    if (!saving)
    {
      if (fd_count == 0 || fd_count > WII_SOCKET_FD_MAX)
      {
        pending_poll_count = 0;
        p.SetMeasureMode();
        return;
      }
      pcmd.wii_fd_count = fd_count;
      pcmd.timeout = 0;
    }
    p.DoArray(pcmd.wii_fds, fd_count);
  }

  if (saving)
    return;
  for (u32 i = 0; i < pending_poll_count; i++)
  {
    PollCommand& pcmd = pending_polls[i];
    for (u32 j = 0; j < pcmd.wii_fd_count; j++)
      pcmd.wii_fds[j].revents = (POLLHUP | POLLERR);
  }
}

bool WiiSockMan::AddPollCommand(const PollCommand& cmd)
{
  if (pending_poll_count == MAX_PENDING_POLLS)
  {
    ERROR_LOG(IOS_NET, "Too many pending polls");
    return false;
  }
  pending_polls[pending_poll_count++] = cmd;
  return true;
}

void WiiSockMan::UpdateWantDeterminism(bool want)
{
  // If we switched into movie recording, kill existing sockets.
  if (want)
    Clean();
}

void WiiSocket::Abort(WiiSocket::sockop* op, s32 value) const
{
  op->is_aborted = true;
  m_socket_manager.EnqueueIPCReply(op->request, value);
}
#undef ERRORCODE
#undef EITHER
}  // namespace IOS::HLE
