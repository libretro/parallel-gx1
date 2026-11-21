// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"

#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{
enum class JVSIOStatusCode : u8;
enum class JVSIOReportCode : u8;
enum class JVSIOCommand : u8;

class JVSIORequestReader;
class JVSIOResponseWriter;

// "JAMMA Video Standard" I/O
class JVSIOBoard : public SerialDevice
{
public:
  explicit JVSIOBoard(IOPorts* io_ports);

  void Update() override;

  void DoState(PointerWrap& p) override;

protected:
  struct FrameContext
  {
    JVSIORequestReader& request;
    JVSIOResponseWriter& response;
  };

  // The command's report code into *report; false when the command is unknown, which fails the
  // whole frame
  virtual bool HandleCommand(JVSIOCommand cmd, FrameContext ctx, JVSIOReportCode* report);

private:
  void ProcessBroadcastRequest(JVSIORequestReader* request);
  void ProcessUnicastRequest(JVSIORequestReader* request);

  void WriteResponse(const u8* response, std::size_t size);

  // 0 == address not yet assigned.
  u8 m_client_address = 0;

  // Note: Does not include JVSIO_SYNC.
  // A frame is at most 3 header bytes, 255 more and a checksum; the rest is room for a frame
  // that overflows, which EndFrame cuts back to its header.
  static constexpr std::size_t MAX_RESPONSE_SIZE = 1024;
  u8 m_last_response[MAX_RESPONSE_SIZE] = {};
  std::size_t m_last_response_size = 0;

  u16 m_coin_counts[IOPorts::COIN_SLOT_COUNT] = {};
  bool m_coin_prev_states[IOPorts::COIN_SLOT_COUNT] = {};

  IOPorts* const m_io_ports;
};

// Used by MarioKartGP and MarioKartGP2.
class Namco_FCA_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  bool HandleCommand(JVSIOCommand cmd, FrameContext ctx, JVSIOReportCode* report) override;
};

// Used by VirtuaStriker4 and others.
class Sega_837_13551_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  bool HandleCommand(JVSIOCommand cmd, FrameContext ctx, JVSIOReportCode* report) override;
};

// Used by FZeroAX (Deluxe).
class Sega_837_13844_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  bool HandleCommand(JVSIOCommand cmd, FrameContext ctx, JVSIOReportCode* report) override;
};

}  // namespace Triforce
