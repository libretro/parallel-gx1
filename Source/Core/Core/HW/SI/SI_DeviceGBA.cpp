// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/SI/SI_DeviceGBA.h"

#include <retro_miscellaneous.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <queue>

#include <queues/mpsc_stack.h>

#include "Common/FrameClock.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/NetSocket.h"
#include "Common/Thread.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace SerialInterface
{
namespace
{
sthread_t* s_connection_thread = nullptr;
// Accepted sockets, pushed by the connection thread and taken by the CPU thread, which keeps
// its own queue of them oldest first.
struct WaitingSock
{
  mpsc_stack_node_t link;  // first: the drained chain is cast back
  int socket;
};
mpsc_stack_t s_waiting_socks;
mpsc_stack_t s_waiting_clocks;
std::queue<int> s_taken_socks;
std::queue<int> s_taken_clocks;

void PushSock(mpsc_stack_t* stack, int socket)
{
  auto* waiting = new WaitingSock{{}, socket};
  mpsc_stack_push(stack, &waiting->link);
}

int TakeSock(mpsc_stack_t* stack, std::queue<int>& taken)
{
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(stack)); node;)
  {
    auto* waiting = reinterpret_cast<WaitingSock*>(node);
    node = node->next;
    taken.push(waiting->socket);
    delete waiting;
  }
  if (taken.empty())
    return -1;
  const int socket = taken.front();
  taken.pop();
  return socket;
}
int s_num_connected;
Common::Flag s_server_running;
}  // namespace

constexpr auto SEND_MAX_SIZE = 5, RECV_MAX_SIZE = 5;

// --- GameBoy Advance "Link Cable" ---

static void GBAConnectionWaiter(void*)
{
  s_server_running.Set();

  Common::SetCurrentThreadName("GBA Connection Waiter");

  // "dolphin gba"
  const int server = net_tcp_listen(0xd6ba);
  if (server < 0)
    return;

  // "clock"
  const int clock_server = net_tcp_listen(0xc10c);
  if (clock_server < 0)
  {
    net_close(server);
    return;
  }

  while (s_server_running.IsSet())
  {
    const int new_client = net_tcp_accept(server);
    if (new_client >= 0)
      PushSock(&s_waiting_socks, new_client);
    const int new_clock = net_tcp_accept(clock_server);
    if (new_clock >= 0)
      PushSock(&s_waiting_clocks, new_clock);

    // Look for a connection once a frame.
    frame_clock_wait_until(frame_clock_now() + 1, s_server_running.Raw());
  }
  net_close(server);
  net_close(clock_server);
}

void GBAConnectionWaiter_Shutdown()
{
  s_server_running.Clear();
  frame_clock_wake_all();
  Common::JoinThread(s_connection_thread);
}

static int GetNextSock()
{
  return TakeSock(&s_waiting_socks, s_taken_socks);
}

static int GetNextClock()
{
  return TakeSock(&s_waiting_clocks, s_taken_clocks);
}

GBASockServer::GBASockServer()
{
  if (!s_connection_thread)
    s_connection_thread = sthread_create(GBAConnectionWaiter, nullptr);

  s_num_connected = 0;
}

GBASockServer::~GBASockServer()
{
  Disconnect();
}

void GBASockServer::Disconnect()
{
  if (m_client >= 0)
  {
    s_num_connected--;
    net_close(m_client);
    m_client = -1;
  }
  if (m_clock_sync >= 0)
  {
    net_close(m_clock_sync);
    m_clock_sync = -1;
  }
  m_last_time_slice = 0;
  m_booted = false;
}

void GBASockServer::ClockSync(Core::System& system)
{
  if (m_clock_sync < 0)
    if ((m_clock_sync = GetNextClock()) < 0)
      return;

  const auto& core_timing = system.GetCoreTiming();

  u32 time_slice = 0;

  if (m_last_time_slice == 0)
  {
    s_num_connected++;
    m_last_time_slice = core_timing.GetTicks();
    time_slice = (u32)(system.GetSystemTimers().GetTicksPerSecond() / 60);
  }
  else
  {
    time_slice = (u32)(core_timing.GetTicks() - m_last_time_slice);
  }

  time_slice = (u32)((u64)time_slice * 16777216 / system.GetSystemTimers().GetTicksPerSecond());
  m_last_time_slice = core_timing.GetTicks();
  char bytes[4] = {0, 0, 0, 0};
  bytes[0] = (time_slice >> 24) & 0xff;
  bytes[1] = (time_slice >> 16) & 0xff;
  bytes[2] = (time_slice >> 8) & 0xff;
  bytes[3] = time_slice & 0xff;

  if (net_tcp_send(m_clock_sync, bytes, 4) < 0)
  {
    net_close(m_clock_sync);
    m_clock_sync = -1;
  }
}

bool GBASockServer::Connect()
{
  if (!IsConnected())
  {
    m_client = GetNextSock();
    if (m_client >= 0)
      net_set_blocking(m_client, false);
  }
  return IsConnected();
}

bool GBASockServer::IsConnected()
{
  return m_client >= 0;
}

void GBASockServer::Send(const u8* si_buffer)
{
  if (!Connect())
    return;

  std::array<u8, SEND_MAX_SIZE> send_data;
  for (size_t i = 0; i < send_data.size(); i++)
    send_data[i] = si_buffer[i];

  const auto cmd = static_cast<EBufferCommands>(send_data[0]);

  const size_t size = cmd == EBufferCommands::CMD_WRITE_GBA ? send_data.size() : 1;
  if (net_tcp_send(m_client, send_data.data(), size) < 0)
    Disconnect();
}

int GBASockServer::Receive(u8* si_buffer, u8 bytes)
{
  if (m_client < 0)
    return 0;

  if (m_booted)
    (void)net_wait_readable(m_client, 1000);

  std::array<u8, RECV_MAX_SIZE> recv_data;
  const int received = net_tcp_recv(m_client, recv_data.data(), bytes);
  if (received < 0)
  {
    Disconnect();
    return 0;
  }
  const size_t num_received = static_cast<size_t>(received);

  if (num_received == 0)
  {
    m_booted = false;
    return 0;
  }
  m_booted = true;

  for (size_t i = 0; i < recv_data.size(); i++)
    si_buffer[i] = recv_data[i];
  return static_cast<int>(MIN(num_received, recv_data.size()));
}

void GBASockServer::Flush()
{
  if (m_client < 0)
    return;

  u8 byte;
  while (net_tcp_recv(m_client, &byte, 1) > 0)
  {
  }
}

CSIDevice_GBA::CSIDevice_GBA(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
}

int CSIDevice_GBA::RunBuffer(u8* buffer, int request_length)
{
  switch (m_next_action)
  {
  case NextAction::SendCommand:
  {
    m_sock_server.ClockSync(m_system);
    if (m_sock_server.Connect())
    {
#ifdef _DEBUG
      NOTICE_LOG(SERIALINTERFACE, "%d cmd %02x [> %02x%02x%02x%02x]", m_device_number, buffer[0],
                 buffer[1], buffer[2], buffer[3], buffer[4]);
#endif
      m_sock_server.Flush();  // Clear out any replies we might have timed out waiting for
      m_sock_server.Send(buffer);
    }
    else
    {
      return -1;
    }

    m_last_cmd = static_cast<EBufferCommands>(buffer[0]);
    m_timestamp_sent = m_system.GetCoreTiming().GetTicks();
    m_next_action = NextAction::WaitTransferTime;
    return 0;
  }

  case NextAction::WaitTransferTime:
  {
    const int elapsed_time =
        static_cast<int>(m_system.GetCoreTiming().GetTicks() - m_timestamp_sent);
    // Tell SI to ask again after TransferInterval() cycles
    if (SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_last_cmd) > elapsed_time)
      return 0;
    m_next_action = NextAction::ReceiveResponse;
    [[fallthrough]];
  }

  case NextAction::ReceiveResponse:
  {
    u8 bytes = 1;
    switch (m_last_cmd)
    {
    case EBufferCommands::CMD_RESET:
    case EBufferCommands::CMD_STATUS:
      bytes = 3;
      break;
    case EBufferCommands::CMD_READ_GBA:
      bytes = 5;
      break;
    default:
      break;
    }
    const int num_data_received = m_sock_server.Receive(buffer, bytes);

    m_next_action = NextAction::SendCommand;
    if (num_data_received == 0)
      return -1;
#ifdef _DEBUG
    const Common::Log::LogLevel log_level =
        (m_last_cmd == EBufferCommands::CMD_STATUS || m_last_cmd == EBufferCommands::CMD_RESET) ?
            Common::Log::LogLevel::LERROR :
            Common::Log::LogLevel::LWARNING;
    GENERIC_LOG(Common::Log::LogType::SERIALINTERFACE, log_level,
                "%d                              [< %02x%02x%02x%02x%02x] (%d)", m_device_number,
                buffer[0], buffer[1], buffer[2], buffer[3], buffer[4],
                static_cast<int>(num_data_received));
#endif
    return num_data_received;
  }
  }

  // This should never happen, but appease MSVC which thinks it might.
  ERROR_LOG(SERIALINTERFACE, "Unknown state %d\n", static_cast<int>(m_next_action));
  return 0;
}

int CSIDevice_GBA::TransferInterval()
{
  return SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_last_cmd);
}

DataResponse CSIDevice_GBA::GetData(u32& hi, u32& low)
{
  return DataResponse::NoData;
}

void CSIDevice_GBA::SendCommand(u32 command, u8 poll)
{
}
}  // namespace SerialInterface
