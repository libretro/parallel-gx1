// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RetroDeque.h"

class PointerWrap;

namespace Triforce
{

// Base class for devices that attach to the SerialA/B ports on the Triforce Baseboard.
class SerialDevice
{
public:
  SerialDevice() { u8_deque_init(&m_tx_buffer, 256); }
  virtual ~SerialDevice() { u8_deque_free(&m_tx_buffer); }

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
  const u8* GetRxBytes() const { return m_rx_buffer.data(); }
  std::size_t GetRxByteCount() const { return m_rx_buffer.size(); }

  void ConsumeRxBytes(std::size_t count);

  void WriteTxByte(u8 byte) { u8_deque_push_back_growing(&m_tx_buffer, &byte); }
  void WriteTxBytes(const u8* bytes, std::size_t size);

  // Transfer the entirety of other's tx buffer to this tx buffer.
  void PassThroughTxBytes(SerialDevice& other);

private:
  // The stream of bytes from the baseboard to the device.
  // FYI: Current device implementations tend to empty the entire buffer in one go,
  //  so std::vector's O(n) erase-at-front should be a non-issue.
  // The contiguous data of std::vector is convenient for packet parsing.
  std::vector<u8> m_rx_buffer;

  // The stream of bytes from the device to the baseboard.
  // It may be read in chunks, from the front: a ring in one block, made with the device.
  u8_deque_t m_tx_buffer;
};

}  // namespace Triforce
