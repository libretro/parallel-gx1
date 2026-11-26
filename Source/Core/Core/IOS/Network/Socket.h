// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

typedef pollfd pollfd_t;

#define poll WSAPoll
#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))

#elif defined(__linux__) or defined(__APPLE__) or defined(__FreeBSD__) or defined(__NetBSD__) or   \
    defined(__OpenBSD__) or defined(__HAIKU__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct pollfd pollfd_t;
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Network/IP/Top.h"
#include "Core/IOS/Network/SSL.h"

namespace IOS::HLE
{
constexpr int WII_SOCKET_FD_MAX = 24;

enum
{
  SO_MSG_OOB = 0x01,
  SO_MSG_PEEK = 0x02,
  SO_MSG_NONBLOCK = 0x04,
};

enum
{
  SO_SUCCESS,
  SO_E2BIG = 1,
  SO_EACCES,
  SO_EADDRINUSE,
  SO_EADDRNOTAVAIL,
  SO_EAFNOSUPPORT,
  SO_EAGAIN,
  SO_EALREADY,
  SO_EBADF,
  SO_EBADMSG,
  SO_EBUSY,
  SO_ECANCELED,
  SO_ECHILD,
  SO_ECONNABORTED,
  SO_ECONNREFUSED,
  SO_ECONNRESET,
  SO_EDEADLK,
  SO_EDESTADDRREQ,
  SO_EDOM,
  SO_EDQUOT,
  SO_EEXIST,
  SO_EFAULT,
  SO_EFBIG,
  SO_EHOSTUNREACH,
  SO_EIDRM,
  SO_EILSEQ,
  SO_EINPROGRESS,
  SO_EINTR,
  SO_EINVAL,
  SO_EIO,
  SO_EISCONN,
  SO_EISDIR,
  SO_ELOOP,
  SO_EMFILE,
  SO_EMLINK,
  SO_EMSGSIZE,
  SO_EMULTIHOP,
  SO_ENAMETOOLONG,
  SO_ENETDOWN,
  SO_ENETRESET,
  SO_ENETUNREACH,
  SO_ENFILE,
  SO_ENOBUFS,
  SO_ENODATA,
  SO_ENODEV,
  SO_ENOENT,
  SO_ENOEXEC,
  SO_ENOLCK,
  SO_ENOLINK,
  SO_ENOMEM,
  SO_ENOMSG,
  SO_ENOPROTOOPT,
  SO_ENOSPC,
  SO_ENOSR,
  SO_ENOSTR,
  SO_ENOSYS,
  SO_ENOTCONN,
  SO_ENOTDIR,
  SO_ENOTEMPTY,
  SO_ENOTSOCK,
  SO_ENOTSUP,
  SO_ENOTTY,
  SO_ENXIO,
  SO_EOPNOTSUPP,
  SO_EOVERFLOW,
  SO_EPERM,
  SO_EPIPE,
  SO_EPROTO,
  SO_EPROTONOSUPPORT,
  SO_EPROTOTYPE,
  SO_ERANGE,
  SO_EROFS,
  SO_ESPIPE,
  SO_ESRCH,
  SO_ESTALE,
  SO_ETIME,
  SO_ETIMEDOUT,
  SO_ETXTBSY,
  SO_EXDEV
};

#pragma pack(push, 1)
struct WiiInAddr
{
  u32 addr;
};

struct WiiSockAddr
{
  u8 len;
  u8 family;
  u8 data[6];
};

struct WiiSockAddrIn
{
  u8 len;
  u8 family;
  u16 port;
  WiiInAddr addr;
};
#pragma pack(pop)

class WiiSockMan;

class WiiSocket
{
public:
  explicit WiiSocket(WiiSockMan& socket_manager) : m_socket_manager(socket_manager) {}
  WiiSocket(const WiiSocket&) = delete;
  WiiSocket(WiiSocket&&) = delete;
  ~WiiSocket();
  WiiSocket& operator=(const WiiSocket&) = delete;
  WiiSocket& operator=(WiiSocket&&) = delete;

private:
  using Timeout = TimePoint;
  struct sockop
  {
    Request request;
    bool is_ssl;
    bool is_aborted = false;
    union
    {
      NET_IOCTL net_type;
      SSL_IOCTL ssl_type;
    };
  };

  enum class ConnectingState
  {
    None,
    Connecting,
    Connected,
    Error
  };

  friend class WiiSockMan;
  void SetFd(s32 s);
  void SetWiiFd(s32 s);
  s32 Shutdown(u32 how);
  s32 CloseFd();
  s32 FCntl(u32 cmd, u32 arg);
  void Abort(sockop* op, s32 value) const;

  const Timeout& GetTimeout();
  void ResetTimeout();

  void DoSock(const Request& request, NET_IOCTL type);
  void DoSock(const Request& request, SSL_IOCTL type);
  // The pending operations, in the order they were queued.
  sockop* GetSockop(u32 index);
  bool AddSockop(const sockop& op);
  void RemoveSockop(u32 index);
  void Update(bool read, bool write, bool except);
  void UpdateConnectingState(s32 connect_rv);
  ConnectingState GetConnectingState() const;
  bool IsValid() const { return fd >= 0; }
  bool IsTCP() const;

  WiiSockMan& m_socket_manager;

  s32 fd = -1;
  s32 wii_fd = -1;
  bool nonBlock = false;
  ConnectingState connecting_state = ConnectingState::None;

  // A Request has no default constructor, so the operations are built in place.
  static constexpr u32 MAX_PENDING_SOCKOPS = 32;
  alignas(sockop) unsigned char pending_sockop_storage[MAX_PENDING_SOCKOPS][sizeof(sockop)];
  u32 pending_sockop_count = 0;

  bool has_timeout = false;
  Timeout timeout;
};

class WiiSockMan
{
public:
  enum class ConvertDirection
  {
    WiiToNative,
    NativeToWii
  };

  struct PollCommand
  {
    u32 request_addr = 0;
    u32 buffer_out = 0;
    pollfd_t wii_fds[WII_SOCKET_FD_MAX] = {};
    u32 wii_fd_count = 0;
    s64 timeout = 0;
  };

  explicit WiiSockMan(EmulationKernel& ios);
  WiiSockMan(const WiiSockMan&) = delete;
  WiiSockMan& operator=(const WiiSockMan&) = delete;
  WiiSockMan(WiiSockMan&&) = delete;
  WiiSockMan& operator=(WiiSockMan&&) = delete;
  ~WiiSockMan();

  s32 GetNetErrorCode(s32 ret, const char* caller, bool is_rw);

  void Update();
  static sockaddr_in ToNativeAddrIn(WiiSockAddrIn from);
  static WiiSockAddrIn ToWiiAddrIn(const sockaddr_in& from,
                                   socklen_t addrlen = sizeof(WiiSockAddrIn));
  static s32 ConvertEvents(s32 events, ConvertDirection dir);

  void DoState(PointerWrap& p);
  // Returns false when too many polls are pending.
  bool AddPollCommand(const PollCommand& cmd);
  // NON-BLOCKING FUNCTIONS
  s32 NewSocket(s32 af, s32 type, s32 protocol);
  s32 AddSocket(s32 fd, bool is_rw);
  s32 GetHostSocket(s32 wii_fd) const;
  s32 ShutdownSocket(s32 wii_fd, u32 how);
  s32 DeleteSocket(s32 wii_fd);
  s32 GetLastNetError() const { return errno_last; }
  void SetLastNetError(s32 error) { errno_last = error; }
  void Clean();
  void EnqueueIPCReply(const Request& request, s32 return_value) const;
  template <typename T>
  void DoSock(s32 sock, const Request& request, T type)
  {
    WiiSocket* const socket = FindSocket(sock);
    if (!socket)
    {
      ERROR_LOG(IOS_NET, "DoSock: Error, fd not found (%08x, %08X, %08X)",
                static_cast<unsigned>(sock), static_cast<unsigned>(request.address),
                static_cast<unsigned>(std::to_underlying(type)));
      EnqueueIPCReply(request, -SO_EBADF);
    }
    else
    {
      socket->DoSock(request, type);
    }
  }

private:
  void UpdatePollCommands();
  WiiSocket* FindSocket(s32 wii_fd);
  const WiiSocket* FindSocket(s32 wii_fd) const;
  void DestroySocket(s32 wii_fd);

  friend class WiiSocket;

  EmulationKernel& m_ios;
  // The sockets by Wii descriptor, built in their slots, which are in use while built.
  alignas(WiiSocket) unsigned char m_socket_storage[WII_SOCKET_FD_MAX][sizeof(WiiSocket)];
  bool m_socket_used[WII_SOCKET_FD_MAX] = {};
  s32 errno_last = 0;
  static constexpr u32 MAX_PENDING_POLLS = 16;
  PollCommand pending_polls[MAX_PENDING_POLLS];
  u32 pending_poll_count = 0;
  TimePoint last_time = Clock::now();
};
}  // namespace IOS::HLE
