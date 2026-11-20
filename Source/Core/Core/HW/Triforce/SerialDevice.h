// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Common/RetroDeque.h"

class PointerWrap;

namespace Triforce
{

// Base class for devices that attach to the SerialA/B ports on the Triforce Baseboard.
class SerialDevice
{
public:
  SerialDevice();
  virtual ~SerialDevice();

  SerialDevice(const SerialDevice&) = delete;
  SerialDevice& operator=(const SerialDevice&) = delete;
  SerialDevice(SerialDevice&&) = delete;
  SerialDevice& operator=(SerialDevice&&) = delete;

  void WriteRxBytes(const u8* bytes, std::size_t size);

  std::size_t GetTxByteCount() const { return u8_deque_count(&m_tx_buffer); }

  // Caller should ensure GetTxByteCount() >= size.
  void TakeTxBytes(u8* bytes, std::size_t size);

  virtual void Update() = 0;

  virtual void DoState(PointerWrap& p);

protected:
  // Pending received bytes, lent until the next Rx write or consume.
  const u8* GetRxBytes() const { return m_rx_buffer + m_rx_start; }
  std::size_t GetRxByteCount() const { return m_rx_end - m_rx_start; }

  void ConsumeRxBytes(std::size_t count);

  void WriteTxByte(u8 byte) { u8_deque_push_back_growing(&m_tx_buffer, &byte); }
  void WriteTxBytes(const u8* bytes, std::size_t size);

  // Transfer the entirety of other's tx buffer to this tx buffer.
  void PassThroughTxBytes(SerialDevice& other);

private:
  // The stream of bytes from the baseboard to the device.
  // Pending bytes are m_rx_buffer[m_rx_start, m_rx_end), kept contiguous for packet parsing.
  // Consuming only moves m_rx_start; a write slides what is left to the front when it needs the
  // room, and the buffer only grows when the pending bytes outgrow it.
  u8* m_rx_buffer = nullptr;
  std::size_t m_rx_start = 0;
  std::size_t m_rx_end = 0;
  std::size_t m_rx_capacity = 0;

  // The stream of bytes from the device to the baseboard.
  // It may be read in chunks, from the front: a ring in one block, made with the device.
  u8_deque_t m_tx_buffer;
};

}  // namespace Triforce
