// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/SocketContext.h"

namespace ExpansionInterface
{

class TAPServerConnection
{
public:
  // Called on the read thread with each received frame; data is only valid during the call.
  typedef void (*RecvCallback)(void* user, const u8* data, std::size_t size);

  TAPServerConnection(const char* destination, RecvCallback recv_cb, void* recv_user,
                      std::size_t max_frame_size);
  ~TAPServerConnection();
  TAPServerConnection(const TAPServerConnection&) = delete;
  TAPServerConnection& operator=(const TAPServerConnection&) = delete;

  bool Activate();
  void Deactivate();
  bool IsActivated();
  bool RecvInit();
  void RecvStart();
  void RecvStop();
  // Sends every complete HDLC frame in send_buf and moves what is left to its start.
  bool SendAndRemoveAllHDLCFrames(u8* send_buf, std::size_t* send_size);
  bool SendFrame(const u8* frame, u32 size);

private:
  char* m_destination;  // owned
  const RecvCallback m_recv_cb;
  void* const m_recv_user;
  const std::size_t m_max_frame_size;
  u8* m_frame_buffer;  // owned, m_max_frame_size bytes: the frame being read
  Common::SocketContext m_socket_context;

  int m_fd = -1;
  sthread_t* m_read_thread = nullptr;
  Common::Flag m_read_enabled;
  Common::Flag m_read_shutdown;

  bool StartReadThread();
  void ReadThreadHandler();
};

}  // namespace ExpansionInterface
