// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetworkCaptureLogger.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <cstdio>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Network.h"
#include "Common/PcapFile.h"
#include "Common/ScopeGuard.h"
#include "Common/TimeUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

namespace Core
{
static NetworkCaptureLogger s_none_logger(NetworkCaptureType::None);
static NetworkCaptureLogger s_raw_logger(NetworkCaptureType::Raw);
static NetworkCaptureLogger s_pcap_logger(NetworkCaptureType::PCAP);

NetworkCaptureLogger* NetworkLogger()
{
  if (Config::Get(Config::MAIN_NETWORK_DUMP_AS_PCAP))
  {
    s_pcap_logger.OpenPCAP();
    return &s_pcap_logger;
  }
  if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ) ||
      Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
  {
    return &s_raw_logger;
  }
  return &s_none_logger;
}

void ResetNetworkLogger()
{
  s_pcap_logger.Close();
}

void NetworkCaptureLogger::OpenPCAP()
{
  char stamp[32] = "";
  // the first thread here opens it; packets queued meanwhile wait for it
  if (retro_atomic_load_acquire_int(&m_file_state) != PCAP_CLOSED ||
      !retro_atomic_cas_int(&m_file_state, PCAP_CLOSED, PCAP_OPENING))
  {
    return;
  }
  std::tm local_time;
  if (Common::LocalTime(std::time(nullptr), &local_time))
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %Hh%Mm%Ss", &local_time);
  const std::string filepath =
      File::GetUserPath(D_DUMPSSL_IDX) + SConfig::GetInstance().GetGameID() + " " + stamp + ".pcap";
  m_file.Open(filepath.c_str(), Common::PCAP::LinkType::Ethernet, File::SharedAccess::Read);
  retro_atomic_store_release_int(&m_file_state, PCAP_OPEN);
}

void NetworkCaptureLogger::Close()
{
  // Nothing logs any more; write what is still queued.
  WriteQueued();
  m_file.Close();
  retro_atomic_store_release_int(&m_file_state, PCAP_CLOSED);
  for (u32 i = 0; i <= SOCKET_SEQUENCES; i++)
    m_sequences[i].used = false;
}

/* The socket's entry, made if it has none */
NetworkCaptureLogger::SocketSequence* NetworkCaptureLogger::Sequence(s32 socket)
{
  u32 i, slot = static_cast<u32>(socket) * 2654435761u % SOCKET_SEQUENCES;
  for (i = 0; i < SOCKET_SEQUENCES; i++, slot = (slot + 1) % SOCKET_SEQUENCES)
  {
    SocketSequence* const entry = &m_sequences[slot];
    if (entry->used && entry->socket == socket)
      return entry;
    if (!entry->used)
    {
      entry->used = true;
      entry->socket = socket;
      entry->read = 0;
      entry->write = 0;
      return entry;
    }
  }
  return &m_sequences[SOCKET_SEQUENCES];
}

void NetworkCaptureLogger::OnNewSocket(s32 socket)
{
  if (m_type != NetworkCaptureType::PCAP)
    return;
  SocketSequence* const entry = Sequence(socket);
  entry->read = 0;
  entry->write = 0;
}

static void AppendRawSSL(const char* suffix, const void* data, std::size_t length)
{
  const std::string filename =
      File::GetUserPath(D_DUMPSSL_IDX) + SConfig::GetInstance().GetGameID() + suffix;
  File::IOFile(filename, "ab").WriteBytes(data, length);
}

void NetworkCaptureLogger::LogSSLRead(const void* data, std::size_t length, s32 socket)
{
  if (m_type == NetworkCaptureType::None || !Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ))
    return;
  if (m_type == NetworkCaptureType::Raw)
    AppendRawSSL("_read.bin", data, length);
  else
    Log(LogType::Read, data, length, socket, nullptr);
}

void NetworkCaptureLogger::LogSSLWrite(const void* data, std::size_t length, s32 socket)
{
  if (m_type == NetworkCaptureType::None || !Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
    return;
  if (m_type == NetworkCaptureType::Raw)
    AppendRawSSL("_write.bin", data, length);
  else
    Log(LogType::Write, data, length, socket, nullptr);
}

void NetworkCaptureLogger::LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from)
{
  if (m_type == NetworkCaptureType::PCAP)
    Log(LogType::Read, data, length, socket, from);
}

void NetworkCaptureLogger::LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to)
{
  if (m_type == NetworkCaptureType::PCAP)
    Log(LogType::Write, data, length, socket, to);
}

void NetworkCaptureLogger::LogBBA(const void* data, std::size_t length)
{
  Packet* packet;
  if (m_type != NetworkCaptureType::PCAP || !Config::Get(Config::MAIN_NETWORK_DUMP_BBA))
    return;

  // CEXIETHERNET's RecvHandlePacket and SendFromDirectFIFO run on different threads.
  packet = NewPacket(length);
  if (!packet)
    return;
  std::memcpy(packet->bytes, data, length);
  Queue(packet);
}

NetworkCaptureLogger::Packet* NetworkCaptureLogger::NewPacket(std::size_t size)
{
  Packet* const packet = static_cast<Packet*>(std::malloc(offsetof(Packet, bytes) + size));
  if (packet)
    packet->size = size;
  return packet;
}

/* Everything queued, written in the order it came */
void NetworkCaptureLogger::WriteQueued()
{
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_packets)); node;)
  {
    Packet* const packet = reinterpret_cast<Packet*>(node);
    node = node->next;
    if (m_file.IsOpen())
      m_file.AddPacket(packet->bytes, packet->size);
    std::free(packet);
  }
}

void NetworkCaptureLogger::Queue(Packet* packet)
{
  mpsc_stack_push(&m_packets, &packet->link);
  // Pairs with the writer's fence: it either sees this packet or this thread sees it gone.
  retro_atomic_thread_fence_seq_cst();

  for (;;)
  {
    if (retro_atomic_load_acquire_int(&m_file_state) != PCAP_OPEN)
      return;  // written once the file is open
    if (!retro_atomic_cas_int(&m_writing, 0, 1))
      return;  // the writer at work takes this packet too
    WriteQueued();
    retro_atomic_store_release_int(&m_writing, 0);
    // A packet queued after the drain, by a thread that saw this one still writing.
    retro_atomic_thread_fence_seq_cst();
    if (mpsc_stack_empty(&m_packets))
      return;
  }
}

void NetworkCaptureLogger::Log(LogType log_type, const void* data, std::size_t length, s32 socket,
                               sockaddr* other)
{
  const auto state = Common::SaveNetworkErrorState();
  Common::ScopeGuard guard([&state] { Common::RestoreNetworkErrorState(state); });
  sockaddr_in sock;
  sockaddr_in peer;
  sockaddr_in* from;
  sockaddr_in* to;
  socklen_t sock_len = sizeof(sock);
  socklen_t peer_len = sizeof(sock);

  if (getsockname(socket, reinterpret_cast<sockaddr*>(&sock), &sock_len) != 0)
    return;

  if (other == nullptr && getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0)
    return;

  if (log_type == LogType::Read)
  {
    from = other ? reinterpret_cast<sockaddr_in*>(other) : &peer;
    to = &sock;
  }
  else
  {
    from = &sock;
    to = other ? reinterpret_cast<sockaddr_in*>(other) : &peer;
  }

  LogIPv4(log_type, static_cast<const u8*>(data), static_cast<u16>(length), socket, *from, *to);
}

void NetworkCaptureLogger::LogIPv4(LogType log_type, const u8* data, u16 length, s32 socket,
                                   const sockaddr_in& from, const sockaddr_in& to)
{
  int socket_type;
  socklen_t option_length = sizeof(int);
  Packet* packet;
  u8* out;

  if (getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socket_type),
                 &option_length) != 0 ||
      (socket_type != SOCK_STREAM && socket_type != SOCK_DGRAM))
  {
    return;
  }

  Common::EthernetHeader ethernet_header(0x800);
  Common::MACAddress mac;
  if (Common::StringToMacAddress(Config::Get(Config::MAIN_WIRELESS_MAC), &mac))
  {
    auto& mac_address =
        (log_type == LogType::Write) ? ethernet_header.source : ethernet_header.destination;
    mac_address = mac;
  }

  if (socket_type == SOCK_STREAM)
  {
    SocketSequence* const entry = Sequence(socket);
    u32& sequence_number = (log_type == LogType::Read) ? entry->read : entry->write;
    Common::TCPHeader tcp_header(from, to, sequence_number, data, length);
    sequence_number += static_cast<u32>(length);
    Common::IPv4Header ip_header(tcp_header.Size() + length, tcp_header.IPProto(), from, to);
    packet = NewPacket(ethernet_header.Size() + ip_header.Size() + tcp_header.Size() + length);
    if (!packet)
      return;
    out = packet->bytes;
    std::memcpy(out, &ethernet_header, ethernet_header.Size());
    out += ethernet_header.Size();
    std::memcpy(out, &ip_header, ip_header.Size());
    out += ip_header.Size();
    std::memcpy(out, &tcp_header, tcp_header.Size());
    out += tcp_header.Size();
  }
  else
  {
    Common::UDPHeader udp_header(from, to, length);
    Common::IPv4Header ip_header(udp_header.Size() + length, udp_header.IPProto(), from, to);
    packet = NewPacket(ethernet_header.Size() + ip_header.Size() + udp_header.Size() + length);
    if (!packet)
      return;
    out = packet->bytes;
    std::memcpy(out, &ethernet_header, ethernet_header.Size());
    out += ethernet_header.Size();
    std::memcpy(out, &ip_header, ip_header.Size());
    out += ip_header.Size();
    std::memcpy(out, &udp_header, udp_header.Size());
    out += udp_header.Size();
  }

  std::memcpy(out, data, length);
  Queue(packet);
}
}  // namespace Core
