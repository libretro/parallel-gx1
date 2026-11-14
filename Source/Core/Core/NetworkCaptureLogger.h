// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#ifdef _WIN32
#include <winsock2.h>
using socklen_t = int;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include "Common/CommonTypes.h"
#include "Common/PcapFile.h"

namespace Core
{
#ifdef None
#undef None
#endif
enum class NetworkCaptureType
{
  None,
  Raw,
  PCAP,
};

// What a game sends and receives over the network, captured as the settings ask: nothing, the
// raw SSL streams, or everything as PCAP. One logger per kind of capture, for the whole session.
class NetworkCaptureLogger final
{
public:
  explicit NetworkCaptureLogger(NetworkCaptureType type) : m_type(type) {}
  NetworkCaptureLogger(const NetworkCaptureLogger&) = delete;
  NetworkCaptureLogger& operator=(const NetworkCaptureLogger&) = delete;

  void OnNewSocket(s32 socket);

  void LogSSLRead(const void* data, std::size_t length, s32 socket);
  void LogSSLWrite(const void* data, std::size_t length, s32 socket);

  void LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from);
  void LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to);

  void LogBBA(const void* data, std::size_t length);

  NetworkCaptureType GetCaptureType() const { return m_type; }

  // A PCAP capture's file, opened the first time it is asked for
  void OpenPCAP();
  // Writes what is still queued and closes the file
  void Close();

private:
  enum class LogType
  {
    Read,
    Write,
  };

  // Packets come from the CPU thread, the BBA thread and the network worker. Each one queues
  // its packet, and whichever finds no writer at work becomes it and writes everything
  // queued, its own and the others', so nobody waits on the file.
  struct Packet
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    std::size_t size;
    u8 bytes[1];
  };
  static Packet* NewPacket(std::size_t size);
  void Queue(Packet* packet);
  void WriteQueued();

  // A socket's TCP sequence numbers, by host socket; past the table's room, sockets share one
  struct SocketSequence
  {
    s32 socket;
    u32 read;
    u32 write;
    bool used;
  };
  static constexpr u32 SOCKET_SEQUENCES = 256;
  SocketSequence* Sequence(s32 socket);

  void Log(LogType log_type, const void* data, std::size_t length, s32 socket, sockaddr* other);
  void LogIPv4(LogType log_type, const u8* data, u16 length, s32 socket, const sockaddr_in& from,
               const sockaddr_in& to);

  Common::PCAP m_file;
  mpsc_stack_t m_packets{};
  SocketSequence m_sequences[SOCKET_SEQUENCES + 1]{};
  retro_atomic_int_t m_writing{};
  enum
  {
    PCAP_CLOSED,
    PCAP_OPENING,
    PCAP_OPEN,
  };
  retro_atomic_int_t m_file_state{};
  NetworkCaptureType m_type;
};

// The logger the network settings ask for now.
NetworkCaptureLogger* NetworkLogger();
// Closes what the loggers write to.
void ResetNetworkLogger();
}  // namespace Core
