// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceGecko.h"

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include <cstdio>

#include "Common/CommonTypes.h"
#include "Common/FrameClock.h"
#include "Common/Logging/Log.h"
#include "Common/NetSocket.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/Core.h"

namespace ExpansionInterface
{
u16 GeckoSockServer::server_port;
int GeckoSockServer::client_count;
sthread_t* GeckoSockServer::connectionThread = nullptr;
Common::Flag GeckoSockServer::server_running;
mpsc_stack_t GeckoSockServer::waiting_socks;
mpsc_stack_node_t* GeckoSockServer::taken_socks;
mpsc_stack_node_t* GeckoSockServer::taken_socks_last;

// Bytes held each way; the Gecko moves one at a time, so this is ample.
constexpr size_t GECKO_FIFO_SIZE = 64 * 1024;

GeckoSockServer::GeckoSockServer() : client_running(false)
{
  retro_spsc_init(&send_fifo, GECKO_FIFO_SIZE);
  retro_spsc_init(&recv_fifo, GECKO_FIFO_SIZE);
  if (!connectionThread)
    connectionThread = sthread_create(GeckoConnectionWaiter, nullptr);
}

GeckoSockServer::~GeckoSockServer()
{
  if (clientThread)
  {
    --client_count;

    client_running.Clear();
    frame_clock_wake_all();
    Common::JoinThread(clientThread);
  }

  if (client_count <= 0 && connectionThread)
  {
    server_running.Clear();
    frame_clock_wake_all();
    Common::JoinThread(connectionThread);
  }

  retro_spsc_free(&send_fifo);
  retro_spsc_free(&recv_fifo);
}

void GeckoSockServer::GeckoConnectionWaiter(void*)
{
  Common::SetCurrentThreadName("Gecko Connection Waiter");

  int server = -1;
  server_port = 0xd6ec;  // "dolphin gecko"
  for (int bind_tries = 0; bind_tries <= 10 && !server_running.IsSet(); bind_tries++)
  {
    server = net_tcp_listen(server_port);
    server_running.Set(server >= 0);
    if (!server_running.IsSet())
      server_port++;
  }

  if (!server_running.IsSet())
    return;

  Core::DisplayMessage(
      StringFromFormat("USBGecko: Listening on TCP port %u", static_cast<unsigned>(server_port)),
      5000);

  while (server_running.IsSet())
  {
    const int new_client = net_tcp_accept(server);
    if (new_client >= 0)
    {
      auto* waiting = new WaitingSock{{}, new_client};
      mpsc_stack_push(&waiting_socks, &waiting->link);
    }

    // Look for a connection once a frame.
    frame_clock_wait_until(frame_clock_now() + 1, server_running.Raw());
  }
  net_close(server);
}

bool GeckoSockServer::GetAvailableSock()
{
  bool sock_filled = false;

  // The stack gives the newest first; queue them oldest first, behind those taken before.
  mpsc_stack_node_t* const drained = mpsc_stack_reverse(mpsc_stack_drain(&waiting_socks));
  if (drained)
  {
    if (taken_socks_last)
      taken_socks_last->next = drained;
    else
      taken_socks = drained;
    for (taken_socks_last = drained; taken_socks_last->next;)
      taken_socks_last = taken_socks_last->next;
  }

  if (taken_socks)
  {
    WaitingSock* const taken = reinterpret_cast<WaitingSock*>(taken_socks);
    const int next = taken->socket;
    if (clientThread)
    {
      client_running.Clear();
      Common::JoinThread(clientThread);

      // Both ends are quiet now.
      retro_spsc_clear(&recv_fifo);
      retro_spsc_clear(&send_fifo);
    }
    client = next;
    retro_atomic_store_release_int(&client_gone, 0);
    clientThread = sthread_create(Common::ThreadEntry<&GeckoSockServer::ClientThread>, this);
    client_count++;
    taken_socks = taken_socks->next;
    if (!taken_socks)
      taken_socks_last = nullptr;
    delete taken;
    sock_filled = true;
  }

  return sock_filled;
}

void GeckoSockServer::ClientThread()
{
  client_running.Set();

  Common::SetCurrentThreadName("Gecko Client");

  net_set_blocking(client, false);

  while (client_running.IsSet())
  {
    bool did_nothing = true;

    {
      // what's an ideal buffer size?
      std::array<char, 128> buffer;
      const int got = net_tcp_recv(client, buffer.data(), buffer.size());
      if (got < 0)
        client_running.Clear();

      if (got > 0)
      {
        did_nothing = false;

        // A full ring drops the excess; a guest that stopped reading would never take it.
        retro_spsc_write(&recv_fifo, buffer.data(), got);
      }

      if (const size_t pending = retro_spsc_read_avail(&send_fifo))
      {
        did_nothing = false;

        std::vector<char> packet(pending);
        retro_spsc_read(&send_fifo, packet.data(), pending);

        if (net_tcp_send(client, &packet[0], packet.size()) < 0)
          client_running.Clear();
      }
    }

    // Idle: look again next frame.
    if (did_nothing)
      frame_clock_wait_until(frame_clock_now() + 1, client_running.Raw());
  }

  net_close(client);
  retro_atomic_store_release_int(&client_gone, 1);
}

CEXIGecko::CEXIGecko(Core::System& system) : IEXIDevice(system)
{
}

void CEXIGecko::ImmReadWrite(u32& _uData, u32 _uSize)
{
  // We don't really care about _uSize
  (void)_uSize;

  if (client < 0 || retro_atomic_load_acquire_int(&client_gone))
    GetAvailableSock();

  switch (_uData >> 28)
  {
  case CMD_LED_OFF:
    Core::DisplayMessage("USBGecko: No LEDs for you!", 3000);
    break;
  case CMD_LED_ON:
    Core::DisplayMessage("USBGecko: A piercing blue light is now shining in your general direction",
                         3000);
    break;

  case CMD_INIT:
    _uData = ident;
    break;

  // PC -> Gecko
  // |= 0x08000000 if successful
  case CMD_RECV:
  {
    u8 byte;
    if (retro_spsc_read(&recv_fifo, &byte, 1) == 1)
      _uData = 0x08000000 | (byte << 16);
    break;
  }

  // Gecko -> PC
  // |= 0x04000000 if successful
  case CMD_SEND:
  {
    const u8 byte = static_cast<u8>(_uData >> 20);
    retro_spsc_write(&send_fifo, &byte, 1);
    _uData = 0x04000000;
    break;
  }

  // Check if ok for Gecko -> PC, or FIFO full
  // |= 0x04000000 if FIFO is not full
  case CMD_CHK_TX:
    _uData = 0x04000000;
    break;

  // Check if data in FIFO for PC -> Gecko, or FIFO empty
  // |= 0x04000000 if data in recv FIFO
  case CMD_CHK_RX:
  {
    _uData = retro_spsc_read_avail(&recv_fifo) ? 0x04000000 : 0;
    break;
  }

  default:
    ERROR_LOG(EXPANSIONINTERFACE, "Unknown USBGecko command %x", _uData);
    break;
  }
}
}  // namespace ExpansionInterface
