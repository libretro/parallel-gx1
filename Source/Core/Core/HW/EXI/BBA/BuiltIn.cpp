// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/BBA/BuiltIn.h"

#include <algorithm>
#include <bit>
#include <optional>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "Common/FrameClock.h"
#include "Common/BitUtils.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NetSocket.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace
{
u64 GetTickCountStd()
{
  return static_cast<u64>(DTToMs(Clock::now()));
}

std::vector<u8> BuildFINFrame(StackRef* ref)
{
  const Common::TCPPacket result(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                                 ref->ack_num, BBA_TCP_FLAG_FIN | BBA_TCP_FLAG_ACK | BBA_TCP_FLAG_RST);

  for (auto& tcp_buf : ref->tcp_buffers)
    tcp_buf.used = false;
  return result.Build();
}

std::vector<u8> BuildAckFrame(StackRef* ref)
{
  const Common::TCPPacket result(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                                 ref->ack_num, BBA_TCP_FLAG_ACK);
  return result.Build();
}

// Change the IP identification and recompute the checksum
void SetIPIdentification(u8* ptr, std::size_t size, u16 value)
{
  if (size < Common::EthernetHeader::SIZE + Common::IPv4Header::SIZE)
    return;

  u8* const ip_ptr = ptr + Common::EthernetHeader::SIZE;
  const u8 ip_header_size = (*ip_ptr & 0xf) * 4;
  if (size < Common::EthernetHeader::SIZE + ip_header_size)
    return;

  u8* const ip_id_ptr = ip_ptr + offsetof(Common::IPv4Header, identification);
  Common::BitCastPtr<u16>(ip_id_ptr) = htons(value);

  u8* const ip_checksum_ptr = ip_ptr + offsetof(Common::IPv4Header, header_checksum);
  auto checksum_bitcast_ptr = Common::BitCastPtr<u16>(ip_checksum_ptr);
  checksum_bitcast_ptr = u16(0);
  checksum_bitcast_ptr = htons(Common::ComputeNetworkChecksum(ip_ptr, ip_header_size));
}
}  // namespace

namespace ExpansionInterface
{
bool CEXIETHERNET::BuiltInBBAInterface::Activate()
{
  if (IsActivated())
    return true;

  m_active = true;
  for (auto& buf : m_queue_data)
    buf.reserve(2048);

  // Workaround to get the host IP (might not be accurate)
  // TODO: Fix the JNI crash and use GetSystemDefaultInterface()
  //  - https://pastebin.com/BFpmnxby (see https://dolp.in/pr10920)
  u32 ip;
  if (!net_resolve_ipv4(m_local_ip.c_str(), &ip) && !net_local_ipv4(&ip))
    ip = 0;
  m_current_ip = ip;
  m_current_mac = Common::BitCastPtr<Common::MACAddress>(&m_eth_ref->mBbaMem[BBA_NAFR_PAR0]);
  m_arp_table[m_current_ip] = m_current_mac;
  m_router_ip = (m_current_ip & 0xFFFFFF) | 0x01000000;
  m_router_mac = Common::GenerateMacAddress(Common::MACConsumer::BBA);
  m_arp_table[m_router_ip] = m_router_mac;

  m_network_ref.Clear();

  m_upnp_httpd = net_tcp_listen_on(ip, Common::SSDP_PORT);

  return RecvInit();
}

void CEXIETHERNET::BuiltInBBAInterface::Deactivate()
{
  // Is the BBA Active? If not skip shutdown
  if (!IsActivated())
    return;
  // Signal read thread to exit.
  m_read_enabled.Clear();
  m_read_thread_shutdown.Set();
  frame_clock_wake_all();
  m_active = false;

  // Wait for read thread to exit; the network stack is this thread's again after.
  Common::JoinThread(m_read_thread);

  RunRequests();
  m_network_ref.Clear();
  m_arp_table.clear();
  net_close(m_upnp_httpd);
  m_upnp_httpd = -1;
}

bool CEXIETHERNET::BuiltInBBAInterface::IsActivated()
{
  return m_active;
}

void CEXIETHERNET::BuiltInBBAInterface::WriteToQueue(std::vector<u8> data)
{
  m_queue_data[m_queue_write] = std::move(data);
  const u8 next_write_index = (m_queue_write + 1) & 15;
  if (next_write_index != m_queue_read)
    m_queue_write = next_write_index;
  else
    WARN_LOG(SP1, "BBA queue overrun, data might be lost");
}

bool CEXIETHERNET::BuiltInBBAInterface::WillQueueOverrun() const
{
  return ((m_queue_write + 1) & 15) == m_queue_read;
}

void CEXIETHERNET::BuiltInBBAInterface::PollData(std::size_t* datasize)
{
  for (auto& net_ref : m_network_ref)
  {
    if (net_ref.ip == 0)
      continue;

    // Check for sleeping TCP data
    if (net_ref.type == IPPROTO_TCP)
    {
      for (auto& tcp_buf : net_ref.tcp_buffers)
      {
        if (WillQueueOverrun())
          break;
        if (!tcp_buf.used || (GetTickCountStd() - tcp_buf.tick) <= 1000)
          continue;

        // Timed out packet, resend
        tcp_buf.tick = GetTickCountStd();
        WriteToQueue(tcp_buf.data);
      }
    }

    // Check for connection data
    if (*datasize == 0)
    {
      // Send it to the network buffer if empty
      const auto socket_data = TryGetDataFromSocket(&net_ref);
      if (socket_data.has_value())
      {
        *datasize = socket_data->size();
        std::memcpy(m_eth_ref->mRecvBuffer.get(), socket_data->data(), *datasize);
      }
    }
    else if (!WillQueueOverrun())
    {
      // Otherwise, enqueue it
      const auto socket_data = TryGetDataFromSocket(&net_ref);
      if (socket_data.has_value())
        WriteToQueue(std::move(*socket_data));
    }
    else
    {
      WARN_LOG(SP1, "BBA queue might overrun, can't poll more data");
      return;
    }
  }
}

void CEXIETHERNET::BuiltInBBAInterface::HandleARP(const Common::ARPPacket& packet)
{
  const auto& [hwdata, arpdata] = packet;
  if (arpdata.sender_address == m_current_mac && arpdata.sender_ip == 0 &&
      arpdata.target_ip == m_current_ip)
  {
    // Ignore ARP probe to itself (RFC 5227) sometimes used to prevent IP collision
    return;
  }
  Common::ARPPacket response(m_current_mac, m_router_mac);
  response.arp_header = Common::ARPHeader(arpdata.target_ip, ResolveAddress(arpdata.target_ip),
                                          m_current_ip, m_current_mac);
  WriteToQueue(response.Build());
}

void CEXIETHERNET::BuiltInBBAInterface::HandleDHCP(const Common::UDPPacket& packet)
{
  const auto& [hwdata, ip, udp_header, ip_options, data] = packet;
  const Common::DHCPPacket dhcp(packet.data.data(), packet.data.size());
  const Common::DHCPBody& request = dhcp.body;
  sockaddr_in from;
  sockaddr_in to;
  from.sin_addr.s_addr = m_router_ip;
  from.sin_family = IPPROTO_UDP;
  from.sin_port = htons(67);
  to.sin_addr.s_addr = m_current_ip;
  to.sin_family = IPPROTO_UDP;
  to.sin_port = udp_header.source_port;

  const u8* router_ip_ptr = reinterpret_cast<const u8*>(&m_router_ip);
  const std::vector<u8> ip_part(router_ip_ptr, router_ip_ptr + sizeof(m_router_ip));

  constexpr auto timeout_24h = std::to_array<u8>({0, 1, 0x51, 0x80});

  Common::DHCPPacket reply;
  reply.body = Common::DHCPBody(request.transaction_id, m_current_mac, m_current_ip, m_router_ip);

  // options
  // send our emulated lan settings

  (dhcp.options.size() == 0 || dhcp.options[0].size() < 2 || dhcp.options[0].at(2) == 1) ?
      reply.AddOption(53, {2}) :  // default, send a suggestion
      reply.AddOption(53, {5});
  reply.AddOption(54, ip_part.data(), ip_part.size());  // dhcp server ip
  reply.AddOption(51, timeout_24h.data(), timeout_24h.size());  // lease time 24h
  reply.AddOption(58, timeout_24h.data(), timeout_24h.size());  // renewal time
  reply.AddOption(59, timeout_24h.data(), timeout_24h.size());  // rebind time
  reply.AddOption(1, {255, 255, 255, 0});  // submask
  reply.AddOption(28, {ip_part[0], ip_part[1], ip_part[2], 255});  // broadcast ip
  reply.AddOption(6, ip_part.data(), ip_part.size());  // dns server
  reply.AddOption(15, {0x6c, 0x61, 0x6e});  // domain name "lan"
  reply.AddOption(3, ip_part.data(), ip_part.size());  // router ip
  reply.AddOption(255, {});  // end

  const Common::UDPPacket response(m_current_mac, m_router_mac, from, to, reply.Build());

  WriteToQueue(response.Build());
}

std::optional<std::vector<u8>>
CEXIETHERNET::BuiltInBBAInterface::TryGetDataFromSocket(StackRef* ref)
{
  std::size_t datasize = 0;
  u16 remote_port = 0;

  switch (ref->type)
  {
  case IPPROTO_UDP:
  {
    std::array<u8, MAX_UDP_LENGTH> buffer;
    u32 remote_ip = 0;
    (void)ref->udp_socket.Receive(buffer.data(), MAX_UDP_LENGTH, &datasize, &remote_ip,
                                  &remote_port);
    if (datasize > 0)
    {
      ref->from.sin_port = htons(remote_port);
      ref->from.sin_addr.s_addr = remote_ip;
      ref->my_mac = ResolveAddress(remote_ip);
      const std::vector<u8> udp_data(buffer.begin(), buffer.begin() + datasize);
      const Common::UDPPacket packet(ref->bba_mac, ref->my_mac, ref->from, ref->to, udp_data);
      return packet.Build();
    }
    break;
  }

  case IPPROTO_TCP:
    switch (ref->tcp_socket.Connected(ref))
    {
    case BbaTcpSocket::ConnectingState::Error:
    {
      // Create the resulting RST ACK packet
      const Common::TCPPacket result(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                                     ref->ack_num, BBA_TCP_FLAG_RST | BBA_TCP_FLAG_ACK);
      WriteToQueue(result.Build());
      ref->ip = 0;
      ref->tcp_socket.Disconnect();
      [[fallthrough]];
    }
    case BbaTcpSocket::ConnectingState::None:
    case BbaTcpSocket::ConnectingState::Connecting:
      return std::nullopt;
    case BbaTcpSocket::ConnectingState::Connected:
      break;
    }

    BbaSocketStatus st = BbaSocketStatus::Done;
    TcpBuffer* tcp_buffer = nullptr;
    for (auto& tcp_buf : ref->tcp_buffers)
    {
      if (tcp_buf.used)
        continue;
      tcp_buffer = &tcp_buf;
      break;
    }

    // set default size to 0 to avoid issue
    datasize = 0;
    const bool can_go = (GetTickCountStd() - ref->poke_time > 100 || ref->window_size > 2000);
    std::array<u8, MAX_TCP_LENGTH> buffer;
    if (tcp_buffer != nullptr && ref->ready && can_go)
      st = ref->tcp_socket.Receive(buffer.data(), MAX_TCP_LENGTH, &datasize);

    if (datasize > 0)
    {
      Common::TCPPacket packet(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                               ref->ack_num, BBA_TCP_FLAG_ACK | BBA_TCP_FLAG_PSH);
      packet.data = std::vector<u8>(buffer.begin(), buffer.begin() + datasize);

      // build buffer
      tcp_buffer->seq_id = ref->seq_num;
      tcp_buffer->tick = GetTickCountStd();
      tcp_buffer->data = packet.Build();
      tcp_buffer->seq_id = ref->seq_num;
      tcp_buffer->used = true;
      ref->seq_num += static_cast<u32>(datasize);
      ref->poke_time = GetTickCountStd();
      return tcp_buffer->data;
    }
    if (GetTickCountStd() - ref->delay > 3000)
    {
      if (st == BbaSocketStatus::Disconnected || st == BbaSocketStatus::Error)
      {
        ref->ip = 0;
        ref->tcp_socket.Disconnect();
        return BuildFINFrame(ref);
      }
    }
    break;
  }

  return std::nullopt;
}

void CEXIETHERNET::BuiltInBBAInterface::HandleTCPFrame(const Common::TCPPacket& packet)
{
  const auto& [hwdata, ip_header, tcp_header, ip_options, tcp_options, data] = packet;
  StackRef* ref = m_network_ref.GetTCPSlot(tcp_header.source_port, tcp_header.destination_port,
                                           std::bit_cast<u32>(ip_header.destination_addr));
  const u16 flags = ntohs(tcp_header.properties) & 0xfff;
  if (flags & (BBA_TCP_FLAG_FIN | BBA_TCP_FLAG_RST))
  {
    if (ref == nullptr)
      return;  // not found

    ref->ack_num += 1 + static_cast<u32>(data.size());
    WriteToQueue(BuildFINFrame(ref));
    ref->ip = 0;
    if (!data.empty())
      (void)ref->tcp_socket.Send(data.data(), data.size());
    ref->tcp_socket.Disconnect();
  }
  else if (flags == (BBA_TCP_FLAG_SIN | BBA_TCP_FLAG_ACK))
  {
    if (ref == nullptr)
      return;  // not found

    ref->seq_num++;
    ref->ack_num = ntohl(tcp_header.sequence_number) + 1;
    ref->ready = true;
    WriteToQueue(BuildAckFrame(ref));
  }
  else if (flags & BBA_TCP_FLAG_SIN)
  {
    // new connection
    if (ref != nullptr)
      return;
    ref = m_network_ref.GetAvailableSlot(0);

    ref->delay = GetTickCountStd();
    ref->local = tcp_header.source_port;
    ref->remote = tcp_header.destination_port;
    ref->ack_num = ntohl(tcp_header.sequence_number) + 1;
    ref->ack_base = ref->ack_num;
    ref->seq_num = 0x1000000;
    ref->window_size = ntohs(tcp_header.window_size);
    ref->type = IPPROTO_TCP;
    for (auto& tcp_buf : ref->tcp_buffers)
      tcp_buf.used = false;
    const u32 destination_ip = std::bit_cast<u32>(ip_header.destination_addr);
    ref->from.sin_addr.s_addr = destination_ip;
    ref->from.sin_port = tcp_header.destination_port;
    ref->to.sin_addr.s_addr = std::bit_cast<u32>(ip_header.source_addr);
    ref->to.sin_port = tcp_header.source_port;
    ref->bba_mac = m_current_mac;
    ref->my_mac = ResolveAddress(destination_ip);
    ref->tcp_socket.SetBlocking(false);
    ref->ready = false;
    ref->ip = std::bit_cast<u32>(ip_header.destination_addr);

    ref->tcp_socket.Connect(destination_ip, ntohs(tcp_header.destination_port), m_current_ip);
  }
  else
  {
    // data packet
    if (ref == nullptr)
      return;  // not found

    const int size =
        ntohs(ip_header.total_len) - ip_header.DefinedSize() - tcp_header.GetHeaderSize();
    const u32 this_seq = ntohl(tcp_header.sequence_number);

    if (size > 0)
    {
      // only if contain data
      if (static_cast<int>(this_seq - ref->ack_num) >= 0 &&
          data.size() >= static_cast<std::size_t>(size))
      {
        (void)ref->tcp_socket.Send(data.data(), size);
        ref->ack_num += size;
      }

      // send ack
      WriteToQueue(BuildAckFrame(ref));
    }
    // update windows size
    ref->window_size = ntohs(tcp_header.window_size);

    // clear any ack data
    if (ntohs(tcp_header.properties) & BBA_TCP_FLAG_ACK)
    {
      const u32 ack_num = ntohl(tcp_header.acknowledgement_number);
      for (auto& tcp_buf : ref->tcp_buffers)
      {
        if (!tcp_buf.used || tcp_buf.seq_id >= ack_num)
          continue;

        Common::PacketView view(tcp_buf.data.data(), tcp_buf.data.size());
        auto tcp_packet = view.GetTCPPacket();  // This is always a tcp packet
        if (!tcp_packet.has_value())            // should never happen but just in case
          continue;

        const u32 seq_end = static_cast<u32>(tcp_buf.seq_id + tcp_packet->data.size());
        if (seq_end <= ack_num)
        {
          tcp_buf.used = false;  // confirmed data received
          if (!ref->ready && !ref->tcp_buffers[0].used)
            ref->ready = true;
          continue;
        }
        // partial data, adjust the packet for next ack
        const u16 ack_size = ack_num - tcp_buf.seq_id;
        tcp_packet->data.erase(tcp_packet->data.begin(), tcp_packet->data.begin() + ack_size);

        tcp_buf.seq_id += ack_size;
        tcp_packet->tcp_header.sequence_number = htonl(tcp_buf.seq_id);
        tcp_buf.data = tcp_packet->Build();
      }
    }
  }
}

// This is a little hack, some games open a UDP port
// and listen to it. We open it on our side manually.
void CEXIETHERNET::BuiltInBBAInterface::InitUDPPort(u16 port)
{
  StackRef* ref = m_network_ref.GetAvailableSlot(htons(port));
  if (ref == nullptr || ref->ip != 0)
    return;
  ref->ip = m_router_ip;  // change for ip
  ref->local = htons(port);
  ref->remote = htons(port);
  ref->type = IPPROTO_UDP;
  ref->bba_mac = m_current_mac;
  ref->my_mac = m_router_mac;
  ref->from.sin_addr.s_addr = 0;
  ref->from.sin_port = htons(port);
  ref->to.sin_addr.s_addr = m_current_ip;
  ref->to.sin_port = htons(port);
  ref->udp_socket.SetBlocking(false);
  if (ref->udp_socket.Bind(port, m_current_ip) != BbaSocketStatus::Done)
  {
    ERROR_LOG(SP1, "Couldn't open UDP socket");
    PanicAlert("Couldn't open port %u. This might stop the game's LAN mode from working properly.",
               (unsigned)port);
    return;
  }
}

void CEXIETHERNET::BuiltInBBAInterface::HandleUDPFrame(const Common::UDPPacket& packet)
{
  const auto& [hwdata, ip_header, udp_header, ip_options, data] = packet;
  u32 target = 0;
  const u32 destination_addr = ip_header.destination_addr == Common::IP_ADDR_ANY ?
                                   m_router_ip :  // dns request
                                   std::bit_cast<u32>(ip_header.destination_addr);

  StackRef* ref = m_network_ref.GetAvailableSlot(udp_header.source_port);
  if (ref->ip == 0)
  {
    ref->ip = destination_addr;  // change for ip
    ref->local = udp_header.source_port;
    ref->remote = udp_header.destination_port;
    ref->type = IPPROTO_UDP;
    ref->bba_mac = m_current_mac;
    ref->my_mac = m_router_mac;
    ref->from.sin_addr.s_addr = destination_addr;
    ref->from.sin_port = udp_header.destination_port;
    ref->to.sin_addr.s_addr = std::bit_cast<u32>(ip_header.source_addr);
    ref->to.sin_port = udp_header.source_port;
    ref->udp_socket.SetBlocking(false);
    if (ref->udp_socket.Bind(ntohs(udp_header.source_port), m_current_ip) !=
        BbaSocketStatus::Done)
    {
      PanicAlert(
          "Port %u is already in use. This might stop the game's LAN mode from working properly.",
          (unsigned)htons(udp_header.source_port));
      if (ref->udp_socket.Bind(0, m_current_ip) != BbaSocketStatus::Done)
      {
        ERROR_LOG(SP1, "Couldn't open UDP socket");
        return;
      }
      if (ntohs(udp_header.destination_port) == Common::SSDP_PORT && ntohs(udp_header.length) > 150)
      {
        // Quick hack to unlock the connection, throw it back at him
        Common::UDPPacket reply = packet;
        reply.eth_header.destination = hwdata.source;
        reply.eth_header.source = hwdata.destination;
        reply.ip_header.destination_addr = ip_header.source_addr;
        reply.ip_header.source_addr = std::bit_cast<Common::IPAddress>(destination_addr);
        WriteToQueue(reply.Build());
      }
    }
  }
  if (ntohs(udp_header.destination_port) == 53)
    // DNS server IP
  {
    if (!net_resolve_ipv4(m_dns_ip.c_str(), &target))
      target = 0;
  }
  else
  {
    target = std::bit_cast<u32>(ip_header.destination_addr);
  }

  (void)ref->udp_socket.Send(data.data(), data.size(), target, ntohs(udp_header.destination_port));
}

void CEXIETHERNET::BuiltInBBAInterface::HandleUPnPClient()
{
  StackRef* ref = m_network_ref.GetAvailableSlot(0);
  if (ref == nullptr || ref->tcp_socket.Accept(m_upnp_httpd) != BbaSocketStatus::Done)
    return;

  if (ref->tcp_socket.GetPeerName(&ref->from) != BbaSocketStatus::Done ||
      ref->tcp_socket.GetSockName(&ref->to) != BbaSocketStatus::Done)
  {
    ERROR_LOG(SP1, "Failed to accept new UPnP client: %s", Common::StrNetworkError());
    return;
  }

  if (m_current_ip == ref->from.sin_addr.s_addr)
  {
    ref->tcp_socket.Disconnect();
    WARN_LOG(SP1, "Ignoring UPnP request to itself");
    return;
  }

  ref->delay = GetTickCountStd();
  ref->ip = ref->from.sin_addr.s_addr;
  ref->local = ref->to.sin_port;
  ref->remote = ref->from.sin_port;
  ref->ack_num = 0;
  ref->ack_base = ref->ack_num;
  ref->seq_num = 0x1000000;
  ref->window_size = 8192;
  ref->type = IPPROTO_TCP;
  for (auto& tcp_buf : ref->tcp_buffers)
    tcp_buf.used = false;
  ref->bba_mac = m_current_mac;
  ref->my_mac = ResolveAddress(ref->from.sin_addr.s_addr);
  ref->tcp_socket.SetBlocking(false);
  ref->ready = false;

  Common::TCPPacket result(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                           ref->ack_num, BBA_TCP_FLAG_SIN);
  // Based on Nintendont packet capture of Mario Kart: Double Dash!!
  result.tcp_options = {
      0x02, 0x04, 0x05, 0xb4,  // Maximum segment size: 1460 bytes
      0x01,                    // NOP
      0x03, 0x03, 0x08,        // Window scale: 8 (multiply by 256)
      0x01, 0x01,              // NOPs
      0x04, 0x02               // SACK permitted
  };
  WriteToQueue(result.Build());
}

const Common::MACAddress& CEXIETHERNET::BuiltInBBAInterface::ResolveAddress(u32 inet_ip)
{
  auto it = m_arp_table.lower_bound(inet_ip);
  if (it != m_arp_table.end() && it->first == inet_ip)
  {
    return it->second;
  }
  else
  {
    return m_arp_table
        .emplace_hint(it, inet_ip, Common::GenerateMacAddress(Common::MACConsumer::BBA))
        ->second;
  }
}

void CEXIETHERNET::BuiltInBBAInterface::Post(Request::Kind kind, std::vector<u8> frame)
{
  auto* request = new Request{{}, kind, std::move(frame)};
  mpsc_stack_push(&m_requests, &request->link);
  frame_clock_wake_all();
}

void CEXIETHERNET::BuiltInBBAInterface::RunRequests()
{
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_requests)); node;)
  {
    auto* request = reinterpret_cast<Request*>(node);
    node = node->next;
    switch (request->kind)
    {
    case Request::Kind::Frame:
      ProcessFrame(request->frame.data(), static_cast<u32>(request->frame.size()));
      break;
    case Request::Kind::Start:
      InitUDPPort(26502);  // Kirby Air Ride
      InitUDPPort(26512);  // Mario Kart: Double Dash!! and 1080° Avalanche
      break;
    case Request::Kind::Stop:
      m_network_ref.Clear();
      m_queue_read = 0;
      m_queue_write = 0;
      break;
    }
    delete request;
  }
}

bool CEXIETHERNET::BuiltInBBAInterface::SendFrame(const u8* frame, u32 size)
{
  // CPU thread: check the frame and hand it to the read thread, which owns the network stack.
  const std::optional<u16> ethertype = Common::PacketView(frame, size).GetEtherType();
  if (!ethertype.has_value())
  {
    ERROR_LOG(SP1, "Unable to send frame with invalid ethernet header");
    return false;
  }
  if (*ethertype != Common::IPV4_ETHERTYPE && *ethertype != Common::ARP_ETHERTYPE)
  {
    ERROR_LOG(SP1, "Unsupported EtherType %#06x", (unsigned)(*ethertype));
    return false;
  }
  Post(Request::Kind::Frame, std::vector<u8>(frame, frame + size));
  m_eth_ref->SendComplete();
  return true;
}

void CEXIETHERNET::BuiltInBBAInterface::ProcessFrame(const u8* frame, u32 size)
{
  const Common::PacketView view(frame, size);

  const std::optional<u16> ethertype = view.GetEtherType();
  if (!ethertype.has_value())
  {
    ERROR_LOG(SP1, "Unable to send frame with invalid ethernet header");
    return;
  }

  switch (*ethertype)
  {
  case Common::IPV4_ETHERTYPE:
  {
    const std::optional<u8> ip_proto = view.GetIPProto();
    if (!ip_proto.has_value())
    {
      ERROR_LOG(SP1, "Unable to send frame with invalid IP header");
      return;
    }

    switch (*ip_proto)
    {
    case IPPROTO_UDP:
    {
      const auto udp_packet = view.GetUDPPacket();
      if (!udp_packet.has_value())
      {
        ERROR_LOG(SP1, "Unable to send frame with invalid UDP header");
        return;
      }

      if (ntohs(udp_packet->udp_header.destination_port) == 67)
      {
        HandleDHCP(*udp_packet);
      }
      else
      {
        HandleUDPFrame(*udp_packet);
      }
      break;
    }

    case IPPROTO_TCP:
    {
      const auto tcp_packet = view.GetTCPPacket();
      if (!tcp_packet.has_value())
      {
        ERROR_LOG(SP1, "Unable to send frame with invalid TCP header");
        return;
      }

      HandleTCPFrame(*tcp_packet);
      break;
    }

    case IPPROTO_IGMP:
    {
      // Acknowledge IGMP packet
      WriteToQueue({frame, frame + size});
      break;
    }

    default:
      ERROR_LOG(SP1, "Unsupported IP protocol %u", (unsigned)(*ip_proto));
      break;
    }
    break;
  }

  case Common::ARP_ETHERTYPE:
  {
    const auto arp_packet = view.GetARPPacket();
    if (!arp_packet.has_value())
    {
      ERROR_LOG(SP1, "Unable to send frame with invalid ARP header");
      return;
    }

    HandleARP(*arp_packet);
    break;
  }

  default:
    ERROR_LOG(SP1, "Unsupported EtherType %#06x", (unsigned)(*ethertype));
    return;
  }

}

void CEXIETHERNET::BuiltInBBAInterface::ReadThreadHandler(void* data)
{
  BuiltInBBAInterface* const self = static_cast<BuiltInBBAInterface*>(data);
  std::size_t datasize = 0;
  while (!self->m_read_thread_shutdown.IsSet())
  {
    self->RunRequests();

    if (datasize == 0)
    {
      // Nothing came in: look again next frame, or sooner when the guest sends (see Post).
      const u64 next = frame_clock_now() + 1;
      const int key = frame_clock_prepare_wait();
      if (self->m_read_thread_shutdown.IsSet() || !mpsc_stack_empty(&self->m_requests) ||
          frame_clock_now() >= next)
        frame_clock_cancel_wait();
      else
        frame_clock_commit_wait(key);
    }

    if (!self->m_read_enabled.IsSet())
      continue;

    u8 wp = self->m_eth_ref->page_ptr(BBA_RWP);
    const u8 rp = self->m_eth_ref->page_ptr(BBA_RRP);
    if (rp > wp)
      wp += 16;

    if ((wp - rp) >= 8)
      continue;

    // process queue file first
    if (self->m_queue_read != self->m_queue_write)
    {
      datasize = self->m_queue_data[self->m_queue_read].size();
      if (datasize > BBA_RECV_SIZE)
      {
        ERROR_LOG(SP1, "Frame size is exceiding BBA capacity, frame stack might be corrupted"
                       "Killing Dolphin...");
        std::exit(0);
      }
      std::memcpy(self->m_eth_ref->mRecvBuffer.get(), self->m_queue_data[self->m_queue_read].data(),
                  datasize);
      self->m_queue_read++;
      self->m_queue_read &= 15;
    }
    else
    {
      datasize = 0;
    }

    // Check network stack references
    self->PollData(&datasize);

    // Check for new UPnP client
    self->HandleUPnPClient();

    if (datasize > 0)
    {
      u8* buffer = reinterpret_cast<u8*>(self->m_eth_ref->mRecvBuffer.get());
      Common::PacketView packet(buffer, datasize);
      const auto packet_type = packet.GetEtherType();
      if (packet_type.has_value() && packet_type == Common::IPV4_ETHERTYPE)
      {
        SetIPIdentification(buffer, datasize, ++self->m_ip_frame_id);
      }
      if (datasize < 64)
      {
        std::fill(buffer + datasize, buffer + 64, 0);
        datasize = 64;
      }
      self->m_eth_ref->mRecvBufferLength = static_cast<u32>(datasize);
      self->m_eth_ref->RecvHandlePacket();
    }
  }
}

bool CEXIETHERNET::BuiltInBBAInterface::RecvInit()
{
  m_read_thread = sthread_create(ReadThreadHandler, this);
  return true;
}

void CEXIETHERNET::BuiltInBBAInterface::RecvStart()
{
  if (m_read_enabled.IsSet())
    return;
  Post(Request::Kind::Start);
  m_read_enabled.Set();
}

void CEXIETHERNET::BuiltInBBAInterface::RecvStop()
{
  m_read_enabled.Clear();
  Post(Request::Kind::Stop);
}
}  // namespace ExpansionInterface

BbaTcpSocket::BbaTcpSocket() = default;

BbaTcpSocket::~BbaTcpSocket()
{
  Disconnect();
}

void BbaTcpSocket::SetBlocking(bool block)
{
  m_blocking = block;
  if (m_fd >= 0)
    net_set_blocking(m_fd, block);
}

void BbaTcpSocket::Disconnect()
{
  net_close(m_fd);
  m_fd = -1;
}

BbaSocketStatus BbaTcpSocket::Connect(u32 dest, u16 port, u32 net_ip)
{
  Disconnect();
  m_fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (m_fd < 0)
    return BbaSocketStatus::Error;
  net_set_blocking(m_fd, m_blocking);

  sockaddr_in addr{};
  addr.sin_addr.s_addr = net_ip;
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  (void)::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  m_connecting_state = ConnectingState::Connecting;

  addr.sin_addr.s_addr = dest;
  addr.sin_port = htons(port);
  if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
    return BbaSocketStatus::Done;
  // a nonblocking connect goes on in the background; Connected() follows it
  return m_blocking ? BbaSocketStatus::Error : BbaSocketStatus::NotReady;
}

BbaSocketStatus BbaTcpSocket::Accept(int listener)
{
  const int fd = net_tcp_accept(listener);
  if (fd < 0)
    return BbaSocketStatus::NotReady;
  Disconnect();
  m_fd = fd;
  net_set_blocking(m_fd, m_blocking);
  return BbaSocketStatus::Done;
}

BbaSocketStatus BbaTcpSocket::Send(const void* data, size_t size)
{
  if (m_fd < 0)
    return BbaSocketStatus::Disconnected;
  const int sent = net_tcp_send(m_fd, data, size);
  if (sent < 0)
    return BbaSocketStatus::Disconnected;
  return sent == 0 && size ? BbaSocketStatus::NotReady : BbaSocketStatus::Done;
}

BbaSocketStatus BbaTcpSocket::Receive(void* data, size_t size, size_t* received)
{
  *received = 0;
  if (m_fd < 0)
    return BbaSocketStatus::Disconnected;
  const int got = net_tcp_recv(m_fd, data, size);
  if (got < 0)
    return BbaSocketStatus::Disconnected;
  if (got == 0)
    return BbaSocketStatus::NotReady;
  *received = static_cast<size_t>(got);
  return BbaSocketStatus::Done;
}

BbaSocketStatus BbaTcpSocket::GetPeerName(sockaddr_in* addr) const
{
  socklen_t size = sizeof(*addr);
  if (getpeername(m_fd, reinterpret_cast<sockaddr*>(addr), &size) == -1)
  {
    ERROR_LOG(SP1, "getpeername failed: %s", Common::StrNetworkError());
    return BbaSocketStatus::Error;
  }
  return BbaSocketStatus::Done;
}

BbaSocketStatus BbaTcpSocket::GetSockName(sockaddr_in* addr) const
{
  socklen_t size = sizeof(*addr);
  if (getsockname(m_fd, reinterpret_cast<sockaddr*>(addr), &size) == -1)
  {
    ERROR_LOG(SP1, "getsockname failed: %s", Common::StrNetworkError());
    return BbaSocketStatus::Error;
  }
  return BbaSocketStatus::Done;
}

BbaTcpSocket::ConnectingState BbaTcpSocket::Connected(StackRef* ref)
{
  // Called by ReadThreadHandler's TryGetDataFromSocket
  switch (m_connecting_state)
  {
  case ConnectingState::Connecting:
  {
    const int fd = m_fd;
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
      ERROR_LOG(SP1, "Failed to get BBA socket connection state: %s", Common::StrNetworkError());
      break;
    }

    if (FD_ISSET(fd, &write_fds) == 0 && FD_ISSET(fd, &except_fds) == 0)
      break;

    s32 error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) != 0)
    {
      ERROR_LOG(SP1, "Failed to get BBA socket error state: %s", Common::StrNetworkError());
      m_connecting_state = ConnectingState::Error;
      break;
    }

    if (error != 0)
    {
      ERROR_LOG(SP1, "BBA connect failed (err=%d): %s", error, Common::DecodeNetworkError(error));
      m_connecting_state = ConnectingState::Error;
      break;
    }

    // Get peername to ensure the socket is connected
    sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0)
    {
      ERROR_LOG(SP1, "BBA connect failed to get peername: %s", Common::StrNetworkError());
      m_connecting_state = ConnectingState::Error;
      break;
    }

    // Create the resulting SYN ACK packet
    m_connecting_state = ConnectingState::Connected;
    INFO_LOG(SP1, "BBA connect succeeded");

    Common::TCPPacket result(ref->bba_mac, ref->my_mac, ref->from, ref->to, ref->seq_num,
                             ref->ack_num, BBA_TCP_FLAG_SIN | BBA_TCP_FLAG_ACK);

    result.tcp_options = {
        0x02, 0x04, 0x05, 0xb4,  // Maximum segment size: 1460 bytes
        0x01, 0x01, 0x01, 0x01   // NOPs
    };

    ref->seq_num++;
    ref->tcp_buffers[0].data = result.Build();
    ref->tcp_buffers[0].seq_id = ref->seq_num - 1;
    ref->tcp_buffers[0].tick = GetTickCountStd() - 900;  // delay
    ref->tcp_buffers[0].used = true;

    break;
  }
  default:
    break;
  }
  return m_connecting_state;
}

BbaUdpSocket::BbaUdpSocket() = default;

BbaUdpSocket::~BbaUdpSocket()
{
  Unbind();
}

void BbaUdpSocket::SetBlocking(bool block)
{
  m_blocking = block;
  if (m_fd >= 0)
    net_set_blocking(m_fd, block);
}

void BbaUdpSocket::Unbind()
{
  net_close(m_fd);
  m_fd = -1;
}

BbaSocketStatus BbaUdpSocket::Send(const void* data, size_t size, u32 dest, u16 port)
{
  if (m_fd < 0)
    return BbaSocketStatus::Error;
  return net_udp_sendto(m_fd, data, size, dest, port) < 0 ? BbaSocketStatus::Error :
                                                            BbaSocketStatus::Done;
}

BbaSocketStatus BbaUdpSocket::Receive(void* data, size_t size, size_t* received, u32* sender,
                                      u16* port)
{
  *received = 0;
  if (m_fd < 0)
    return BbaSocketStatus::Error;
  const int got = net_udp_recvfrom(m_fd, data, size, sender, port);
  if (got < 0)
    return BbaSocketStatus::NotReady;
  *received = static_cast<size_t>(got);
  return BbaSocketStatus::Done;
}

BbaSocketStatus BbaUdpSocket::Bind(u16 port, u32 net_ip)
{
  Unbind();
  m_fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (m_fd < 0)
    return BbaSocketStatus::Error;
  net_set_blocking(m_fd, m_blocking);

  if (port != Common::SSDP_PORT)
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = net_ip;
    addr.sin_port = htons(port);
    if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      Unbind();
      return BbaSocketStatus::Error;
    }
    return BbaSocketStatus::Done;
  }

  // Handle SSDP multicast

  const int on = 1;
  if (setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                 sizeof(on)) != 0)
  {
    ERROR_LOG(SP1, "setsockopt failed to reuse SSDP address: %s", Common::StrNetworkError());
  }
#ifdef SO_REUSEPORT
  if (setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on),
                 sizeof(on)) != 0)
  {
    ERROR_LOG(SP1, "setsockopt failed to reuse SSDP port: %s", Common::StrNetworkError());
  }
#endif
  if (const char loop = 1;
      setsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0)
  {
    ERROR_LOG(SP1, "setsockopt failed to set SSDP loopback: %s", Common::StrNetworkError());
  }

  sockaddr_in addr;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(Common::SSDP_PORT);
  Common::ScopeGuard error_guard([this] { Unbind(); });
  if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    WARN_LOG(SP1, "bind with SSDP port and INADDR_ANY failed: %s", Common::StrNetworkError());
    addr.sin_addr.s_addr = net_ip;
    if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      ERROR_LOG(SP1, "bind with SSDP port failed: %s", Common::StrNetworkError());
      return BbaSocketStatus::Error;
    }
  }
  else
  {
    addr.sin_addr.s_addr = net_ip;  // Set this here for IP_MULTICAST_IF
  }
  INFO_LOG(SP1, "SSDP bind successful");

  // Bind to the right interface
  if (setsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&addr.sin_addr), sizeof(addr.sin_addr)) != 0)
  {
    ERROR_LOG(SP1, "setsockopt failed to bind to the network interface: %s",
              Common::StrNetworkError());
    return BbaSocketStatus::Error;
  }

  // Subscribe to the SSDP multicast group
  // NB: Other groups aren't supported because of HLE
  ip_mreq mreq;
  mreq.imr_multiaddr.s_addr = std::bit_cast<u32>(Common::IP_ADDR_SSDP);
  mreq.imr_interface.s_addr = net_ip;
  if (setsockopt(m_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0)
  {
    ERROR_LOG(SP1, "setsockopt failed to subscribe to SSDP multicast group: %s",
              Common::StrNetworkError());
    return BbaSocketStatus::Error;
  }

  error_guard.Dismiss();
  INFO_LOG(SP1, "SSDP multicast membership successful");
  return BbaSocketStatus::Done;
}

StackRef* NetworkRef::GetAvailableSlot(u16 port)
{
  if (port > 0)  // existing connection?
  {
    for (auto& ref : m_stacks)
    {
      if (ref.ip != 0 && ref.local == port)
        return &ref;
    }
  }
  for (auto& ref : m_stacks)
  {
    if (ref.ip == 0)
      return &ref;
  }
  return nullptr;
}

StackRef* NetworkRef::GetTCPSlot(u16 src_port, u16 dst_port, u32 ip)
{
  for (auto& ref : m_stacks)
  {
    if (ref.ip == ip && ref.remote == dst_port && ref.local == src_port)
    {
      return &ref;
    }
  }
  return nullptr;
}

void NetworkRef::Clear()
{
  for (auto& ref : m_stacks)
  {
    if (ref.ip != 0)
    {
      ref.type == IPPROTO_TCP ? ref.tcp_socket.Disconnect() : ref.udp_socket.Unbind();
    }
    ref.ip = 0;
  }
}
