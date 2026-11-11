// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <winsock2.h>
using socklen_t = int;
#else
#include <netinet/in.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Network.h"

constexpr u16 BBA_TCP_FLAG_SIN = 0x2;
constexpr u16 BBA_TCP_FLAG_ACK = 0x10;
constexpr u16 BBA_TCP_FLAG_PSH = 0x8;
constexpr u16 BBA_TCP_FLAG_FIN = 0x1;
constexpr u16 BBA_TCP_FLAG_RST = 0x4;

constexpr u8 MAX_TCP_BUFFER = 4;
constexpr u16 MAX_UDP_LENGTH = 1500;
constexpr u16 MAX_TCP_LENGTH = 440;

struct TcpBuffer
{
  bool used;
  u64 tick;
  u32 seq_id;
  std::vector<u8> data;
};

struct StackRef;

// Socket helper classes to ensure network interface consistency.
//
// If the socket isn't bound, the system will pick the interface to use automatically.
// This might result in the source IP address changing when talking to the same peer
// and multiple interfaces/IP addresses can reach the socket's peer.
// What a socket call came to.
enum class BbaSocketStatus
{
  Done,
  NotReady,
  Disconnected,
  Error
};

class BbaTcpSocket
{
public:
  BbaTcpSocket();
  ~BbaTcpSocket();
  BbaTcpSocket(const BbaTcpSocket&) = delete;
  BbaTcpSocket& operator=(const BbaTcpSocket&) = delete;

  // Addresses in network order, ports in host order.
  BbaSocketStatus Connect(u32 dest, u16 port, u32 net_ip);
  BbaSocketStatus Accept(int listener);
  BbaSocketStatus GetPeerName(sockaddr_in* addr) const;
  BbaSocketStatus GetSockName(sockaddr_in* addr) const;
  BbaSocketStatus Send(const void* data, size_t size);
  BbaSocketStatus Receive(void* data, size_t size, size_t* received);
  void SetBlocking(bool block);
  void Disconnect();

  enum class ConnectingState
  {
    None,
    Connecting,
    Connected,
    Error
  };

  ConnectingState Connected(StackRef* ref);

private:
  int m_fd = -1;
  bool m_blocking = true;
  ConnectingState m_connecting_state = ConnectingState::None;
};

class BbaUdpSocket
{
public:
  BbaUdpSocket();
  ~BbaUdpSocket();
  BbaUdpSocket(const BbaUdpSocket&) = delete;
  BbaUdpSocket& operator=(const BbaUdpSocket&) = delete;

  // Port 0 is any port.
  BbaSocketStatus Bind(u16 port, u32 net_ip);
  BbaSocketStatus Send(const void* data, size_t size, u32 dest, u16 port);
  BbaSocketStatus Receive(void* data, size_t size, size_t* received, u32* sender, u16* port);
  void SetBlocking(bool block);
  void Unbind();

private:
  int m_fd = -1;
  bool m_blocking = true;
};

struct StackRef
{
  u32 ip = 0;
  u16 local;
  u16 remote;
  u16 type;
  u32 seq_num;
  u32 ack_num;
  u32 ack_base;
  u16 window_size;
  u64 delay;
  std::array<TcpBuffer, MAX_TCP_BUFFER> tcp_buffers;
  bool ready;
  sockaddr_in from;
  sockaddr_in to;
  Common::MACAddress bba_mac{};
  Common::MACAddress my_mac{};
  BbaUdpSocket udp_socket;
  BbaTcpSocket tcp_socket;
  u64 poke_time;
};

// Max 10 at same time, I think most gc game had a
// limit of 8 in the GC framework
using StackRefs = std::array<StackRef, 10>;

class NetworkRef
{
public:
  StackRefs& data() { return m_stacks; }
  const StackRefs& data() const { return m_stacks; }
  auto begin() { return m_stacks.begin(); }
  auto begin() const { return m_stacks.cbegin(); }
  auto end() { return m_stacks.end(); }
  auto end() const { return m_stacks.cend(); }

  StackRef* GetAvailableSlot(u16 port);
  StackRef* GetTCPSlot(u16 src_port, u16 dst_port, u32 ip);

  void Clear();

private:
  StackRefs m_stacks;
};
