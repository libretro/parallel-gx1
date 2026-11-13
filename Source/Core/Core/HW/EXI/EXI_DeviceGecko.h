// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Core/HW/EXI/EXI_Device.h"

namespace ExpansionInterface
{
class GeckoSockServer
{
public:
  GeckoSockServer();
  ~GeckoSockServer();
  bool GetAvailableSock();

  // Client for this server object
  // The connection, which the CPU thread replaces; the client thread closes it and sets
  // client_gone when the peer goes.
  int client = -1;
  retro_atomic_int_t client_gone{};
  void ClientThread();
  sthread_t* clientThread = nullptr;

  // Bytes between the emulated Gecko (CPU thread) and the client thread, one ring each way.
  retro_spsc_t send_fifo;  // CPU thread -> client
  retro_spsc_t recv_fifo;  // client -> CPU thread

private:
  static int client_count;
  Common::Flag client_running;

  // Only ever one server thread
  static void GeckoConnectionWaiter(void*);

  static u16 server_port;
  static Common::Flag server_running;
  static sthread_t* connectionThread;
  // Accepted sockets, pushed by the connection thread and taken by the CPU thread.
  struct WaitingSock
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    int socket;
  };
  static mpsc_stack_t waiting_socks;
  // The CPU thread's own queue of them, oldest first, still in their nodes.
  static mpsc_stack_node_t* taken_socks;
  static mpsc_stack_node_t* taken_socks_last;
};

class CEXIGecko : public IEXIDevice, private GeckoSockServer
{
public:
  explicit CEXIGecko(Core::System& system);
  bool IsPresent() const override { return true; }
  void ImmReadWrite(u32& _uData, u32 _uSize) override;

private:
  enum
  {
    CMD_LED_OFF = 0x7,
    CMD_LED_ON = 0x8,
    CMD_INIT = 0x9,
    CMD_RECV = 0xa,
    CMD_SEND = 0xb,
    CMD_CHK_TX = 0xc,
    CMD_CHK_RX = 0xd,
  };

  static const u32 ident = 0x04700000;
};
}  // namespace ExpansionInterface
