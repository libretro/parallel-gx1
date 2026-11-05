// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceGecko.h"

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Core.h"

namespace ExpansionInterface
{
u16 GeckoSockServer::server_port;
int GeckoSockServer::client_count;
sthread_t* GeckoSockServer::connectionThread = nullptr;
Common::Flag GeckoSockServer::server_running;
mpsc_stack_t GeckoSockServer::waiting_socks;
std::deque<std::unique_ptr<sf::TcpSocket>> GeckoSockServer::taken_socks;

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
    Common::JoinThread(clientThread);
  }

  if (client_count <= 0 && connectionThread)
  {
    server_running.Clear();
    Common::JoinThread(connectionThread);
  }

  retro_spsc_free(&send_fifo);
  retro_spsc_free(&recv_fifo);
}

void GeckoSockServer::GeckoConnectionWaiter(void*)
{
  Common::SetCurrentThreadName("Gecko Connection Waiter");

  sf::TcpListener server;
  server_port = 0xd6ec;  // "dolphin gecko"
  for (int bind_tries = 0; bind_tries <= 10 && !server_running.IsSet(); bind_tries++)
  {
    server_running.Set(server.listen(server_port) == sf::Socket::Status::Done);
    if (!server_running.IsSet())
      server_port++;
  }

  if (!server_running.IsSet())
    return;

  Core::DisplayMessage(fmt::format("USBGecko: Listening on TCP port {}", server_port), 5000);

  server.setBlocking(false);

  auto new_client = std::make_unique<sf::TcpSocket>();
  while (server_running.IsSet())
  {
    if (server.accept(*new_client) == sf::Socket::Status::Done)
    {
      auto* waiting = new WaitingSock{{}, std::move(new_client)};
      mpsc_stack_push(&waiting_socks, &waiting->link);

      new_client = std::make_unique<sf::TcpSocket>();
    }

    Common::SleepCurrentThread(1);
  }
}

bool GeckoSockServer::GetAvailableSock()
{
  bool sock_filled = false;

  // The stack gives the newest first; queue them oldest first.
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&waiting_socks)); node;)
  {
    auto* waiting = reinterpret_cast<WaitingSock*>(node);
    node = node->next;
    taken_socks.push_back(std::move(waiting->socket));
    delete waiting;
  }

  if (!taken_socks.empty())
  {
    client = std::move(taken_socks.front());
    if (clientThread)
    {
      client_running.Clear();
      Common::JoinThread(clientThread);

      // Both ends are quiet now.
      retro_spsc_clear(&recv_fifo);
      retro_spsc_clear(&send_fifo);
    }
    clientThread = sthread_create(Common::ThreadEntry<&GeckoSockServer::ClientThread>, this);
    client_count++;
    taken_socks.pop_front();
    sock_filled = true;
  }

  return sock_filled;
}

void GeckoSockServer::ClientThread()
{
  client_running.Set();

  Common::SetCurrentThreadName("Gecko Client");

  client->setBlocking(false);

  while (client_running.IsSet())
  {
    bool did_nothing = true;

    {
      // what's an ideal buffer size?
      std::array<char, 128> buffer;
      std::size_t got = 0;

      if (client->receive(buffer.data(), buffer.size(), got) == sf::Socket::Status::Disconnected)
        client_running.Clear();

      if (got != 0)
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

        if (client->send(&packet[0], packet.size()) == sf::Socket::Status::Disconnected)
          client_running.Clear();
      }
    }

    if (did_nothing)
      Common::YieldCPU();
  }

  client->disconnect();
}

CEXIGecko::CEXIGecko(Core::System& system) : IEXIDevice(system)
{
}

void CEXIGecko::ImmReadWrite(u32& _uData, u32 _uSize)
{
  // We don't really care about _uSize
  (void)_uSize;

  if (!client || client->getLocalPort() == 0)
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
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Unknown USBGecko command {:x}", _uData);
    break;
  }
}
}  // namespace ExpansionInterface
