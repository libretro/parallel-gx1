// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Logging/Log.h"
#include "Common/NetSocket.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

#include "Core/Core.h"

#include <cstring>
#include <optional>

// BBA implementation with UDP interface to XLink Kai PC/MAC/RaspberryPi client
// For more information please see: https://www.teamxlink.co.uk/wiki/Emulator_Integration_Protocol
// Still have questions? Please email crunchbite@teamxlink.co.uk
// When editing this file please maintain proper capitalization of "XLink Kai"

namespace ExpansionInterface
{
bool CEXIETHERNET::XLinkNetworkInterface::Activate()
{
  if (IsActivated())
    return true;

  // A reader that ended on the client's disconnect has nothing left to do.
  if (m_read_thread)
  {
    Common::JoinThread(m_read_thread);
    m_read_thread = nullptr;
  }

  m_fd = net_udp_open(0);
  if (m_fd < 0)
  {
    ERROR_LOG_FMT(SP1, "Couldn't open XLink Kai UDP socket, unable to initialize BBA");
    return false;
  }

  if (!net_resolve_ipv4(m_dest_ip.c_str(), &m_recipient_ip))
    m_recipient_ip = 0;

  // Send connect command with unique local name
  // connect;locally_unique_name;emulator_name;optional_padding
  u8 buffer[255] = {};
  std::string cmd =
      "connect;" + m_client_identifier + ";dolphin;000000000000000000000000000000000000000000";

  const auto size = u32(cmd.length());
  memmove(buffer, cmd.c_str(), size);

  DEBUG_LOG_FMT(SP1, "SendCommandPayload {:x}\n{}", size, ArrayToString(buffer, size, 0x10));

  if (net_udp_sendto(m_fd, buffer, size, m_recipient_ip, m_dest_port) < 0)
  {
    ERROR_LOG_FMT(SP1, "Activate(): failed to send connect message to XLink Kai client");
  }

  INFO_LOG_FMT(SP1, "BBA initialized.");

  m_read_thread_shutdown.Clear();
  return RecvInit();
}

void CEXIETHERNET::XLinkNetworkInterface::Deactivate()
{
  // Is the BBA Active? If not skip shutdown
  if (!IsActivated())
    return;

  // Send d; to tell XLink we want to disconnect cleanly
  // disconnect;optional_locally_unique_name;optional_padding
  std::string cmd =
      "disconnect;" + m_client_identifier + ";0000000000000000000000000000000000000000000";
  const auto size = u32(cmd.length());
  u8 buffer[255] = {};
  memmove(buffer, cmd.c_str(), size);

  DEBUG_LOG_FMT(SP1, "SendCommandPayload {:x}\n{}", size, ArrayToString(buffer, size, 0x10));

  if (net_udp_sendto(m_fd, buffer, size, m_recipient_ip, m_dest_port) < 0)
  {
    ERROR_LOG_FMT(SP1, "Deactivate(): failed to send disconnect message to XLink Kai client");
  }

  NOTICE_LOG_FMT(SP1, "XLink Kai BBA deactivated");

  m_bba_link_up = false;

  // Set flags for clean shutdown of the read thread
  m_read_enabled.Clear();
  m_read_thread_shutdown.Set();

  // An empty datagram to ourselves ends the reader's receive.
  if (m_read_thread)
  {
    net_udp_sendto(m_fd, "", 0, net_loopback_ipv4(), net_local_port(m_fd));
    Common::JoinThread(m_read_thread);
    m_read_thread = nullptr;
  }

  net_close(m_fd);
  m_fd = -1;
}

bool CEXIETHERNET::XLinkNetworkInterface::IsActivated()
{
  return m_fd >= 0;
}

bool CEXIETHERNET::XLinkNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  // Is the BBA actually connected?
  if (!m_bba_link_up)
  {
    // Has the user been told the connection failed?
    if (!m_bba_failure_notified)
    {
      Core::DisplayMessage("XLink Kai BBA not connected", 30000);
      m_bba_failure_notified = true;
    }

    // Return true because this isn't an error dolphin needs to handle
    return true;
  }

  // Overwrite buffer and add header
  memmove(m_out_frame, "e;e;", 4);
  memmove(m_out_frame + 4, frame, size);
  size += 4;

  // Only uncomment for debugging, the performance hit is too big otherwise
  // INFO_LOG_FMT(SP1, "SendFrame {}\n{}", size, ArrayToString(m_out_frame, size, 0x10)));

  if (net_udp_sendto(m_fd, m_out_frame, size, m_recipient_ip, m_dest_port) < 0)
  {
    ERROR_LOG_FMT(SP1, "SendFrame(): expected to write {} bytes, but failed, errno {}", size,
                  errno);
    return false;
  }
  else
  {
    m_eth_ref->SendComplete();
    return true;
  }
}

void CEXIETHERNET::XLinkNetworkInterface::ReadThreadHandler(void* data)
{
  XLinkNetworkInterface* const self = static_cast<XLinkNetworkInterface*>(data);
  while (!self->m_read_thread_shutdown.IsSet())
  {
    if (!self->IsActivated())
      break;

    // XLink supports jumboframes but GameCube BBA does not. We need to support jumbo frames
    // *here* because XLink *could* send one
    const int received =
        net_udp_recvfrom(self->m_fd, self->m_in_frame, std::size(self->m_in_frame), nullptr, nullptr);
    if (self->m_read_thread_shutdown.IsSet())
      break;
    if (received < 0 && self->m_bba_link_up)
      ERROR_LOG_FMT(SP1, "Failed to read from BBA, err={}", received);

    // Make sure *anything* was received before going any further
    if (received < 1)
      continue;
    const std::size_t bytes_read = static_cast<std::size_t>(received);

    // Did we get an ethernet frame? Copy the first 4 bytes to check the header
    char temp_check[4];
    memmove(temp_check, self->m_in_frame, std::size(temp_check));

    // Check for e;e; header; this indicates the received data is an ethernet frame
    // e;e;raw_ethernet_frame_data
    if (temp_check[2] == 'e' && temp_check[3] == ';')
    {
      // Is the frame larger than BBA_RECV_SIZE?
      if ((bytes_read - 4) < BBA_RECV_SIZE)
      {
        // Copy payload into BBA buffer as an ethernet frame
        memmove(self->m_eth_ref->mRecvBuffer.get(), self->m_in_frame + 4, bytes_read - 4);

        // Check the frame size again after the header is removed
        if (bytes_read < 1)
        {
          ERROR_LOG_FMT(SP1, "Failed to read from BBA, err={}", bytes_read - 4);
        }
        else if (self->m_read_enabled.IsSet())
        {
          // Only uncomment for debugging, the performance hit is too big otherwise
          // DEBUG_LOG_FMT(SP1, "Read data: {}", ArrayToString(self->m_eth_ref->mRecvBuffer.get(),
          // u32(bytes_read - 4), 0x10));
          self->m_eth_ref->mRecvBufferLength = u32(bytes_read - 4);
          self->m_eth_ref->RecvHandlePacket();
        }
      }
    }
    // Otherwise we received control data or junk
    else
    {
      const std::string control_msg(self->m_in_frame, self->m_in_frame + bytes_read);
      INFO_LOG_FMT(SP1, "Received XLink Kai control data: {}", control_msg);

      // connected;identifier;
      if (control_msg.starts_with("connected"))
      {
        NOTICE_LOG_FMT(SP1, "XLink Kai BBA connected");
        Core::DisplayMessage("XLink Kai BBA connected", 4500);

        self->m_bba_link_up = true;
        // TODO (in EXI_DeviceEthernet.cpp) bring the BBA link up here

        // Send any extra settings now
        // Enable XLink chat messages in OSD if set
        if (self->m_chat_osd_enabled)
        {
          constexpr std::string_view cmd = "setting;chat;true;";
          const auto size = u32(cmd.length());
          u8 buffer[255] = {};
          memmove(buffer, cmd.data(), size);

          DEBUG_LOG_FMT(SP1, "SendCommandPayload {:x}\n{}", size,
                        ArrayToString(buffer, size, 0x10));

          if (net_udp_sendto(self->m_fd, buffer, size, self->m_recipient_ip,
                             self->m_dest_port) < 0)
          {
            ERROR_LOG_FMT(
                SP1, "ReadThreadHandler(): failed to send setting message to XLink Kai client");
          }
        }
      }
      // disconnected;optional_identifier;optional_message;
      else if (control_msg.starts_with("disconnected"))
      {
        NOTICE_LOG_FMT(SP1, "XLink Kai BBA disconnected");
        // Show OSD message for 15 seconds to make sure the user sees it
        Core::DisplayMessage("XLink Kai BBA disconnected", 15000);

        // TODO (TODO in EXI_DeviceEthernet.cpp) bring the BBA link down here
        self->m_bba_link_up = false;

        // Shut down the read thread
        self->m_read_enabled.Clear();
        self->m_read_thread_shutdown.Set();

        // Close the socket; the next Activate joins this thread
        net_close(self->m_fd);
        self->m_fd = -1;
        break;
      }
      // keepalive;
      else if (control_msg.starts_with("keepalive"))
      {
        DEBUG_LOG_FMT(SP1, "XLink Kai BBA keepalive");

        // Only uncomment for debugging, just clogs the log otherwise
        // INFO_LOG_FMT(SP1, "SendCommandPayload {:x}\n{}", 2, ArrayToString(m_in_frame, 2, 0x10));

        // Reply (using the message that came in!)
        if (net_udp_sendto(self->m_fd, self->m_in_frame, 10, self->m_recipient_ip,
                           self->m_dest_port) < 0)
        {
          ERROR_LOG_FMT(SP1, "ReadThreadHandler(): failed to reply to XLink Kai client keepalive");
        }
      }
      // message;message_text;
      else if (control_msg.starts_with("message"))
      {
        std::string msg = control_msg.substr(8, control_msg.length() - 1);

        NOTICE_LOG_FMT(SP1, "XLink Kai message: {}", msg);
        // Show OSD message for 15 seconds to make sure the user sees it
        Core::DisplayMessage(std::move(msg), 15000);
      }
      // chat;message_text;
      else if (control_msg.starts_with("chat"))
      {
        std::string msg = control_msg.substr(5, control_msg.length() - 1);

        NOTICE_LOG_FMT(SP1, "XLink Kai chat: {}", msg);
        Core::DisplayMessage(std::move(msg), 5000);
      }
      // directmessage;message_text;
      else if (control_msg.starts_with("directmessage"))
      {
        std::string msg = control_msg.substr(14, control_msg.length() - 1);

        NOTICE_LOG_FMT(SP1, "XLink Kai direct message: {}", msg);
        Core::DisplayMessage(std::move(msg), 5000);
      }
      // else junk/unsupported control message
    }
  }
}

bool CEXIETHERNET::XLinkNetworkInterface::RecvInit()
{
  m_read_thread = sthread_create(ReadThreadHandler, this);
  return true;
}

void CEXIETHERNET::XLinkNetworkInterface::RecvStart()
{
  m_read_enabled.Set();
}

void CEXIETHERNET::XLinkNetworkInterface::RecvStop()
{
  m_read_enabled.Clear();
}
}  // namespace ExpansionInterface
