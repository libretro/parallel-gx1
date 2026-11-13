// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Triforce/SerialDevice.h"


#include "Common/Assert.h"
#include "Common/ChunkFile.h"

namespace Triforce
{

void SerialDevice::WriteRxBytes(const u8* bytes, std::size_t size)
{
  m_rx_buffer.insert(m_rx_buffer.end(), bytes, bytes + size);
}

void SerialDevice::TakeTxBytes(u8* bytes, std::size_t size)
{
  DEBUG_ASSERT(u8_deque_count(&m_tx_buffer) >= size);

  for (std::size_t i = 0; i < size; i++)
  {
    bytes[i] = *u8_deque_front(&m_tx_buffer);
    u8_deque_pop_front(&m_tx_buffer);
  }
}

void SerialDevice::ConsumeRxBytes(std::size_t count)
{
  DEBUG_ASSERT(m_rx_buffer.size() >= count);

  m_rx_buffer.erase(m_rx_buffer.begin(), m_rx_buffer.begin() + count);
}

void SerialDevice::WriteTxBytes(const u8* bytes, std::size_t size)
{
  for (std::size_t i = 0; i < size; i++)
    u8_deque_push_back_growing(&m_tx_buffer, &bytes[i]);
}

void SerialDevice::PassThroughTxBytes(SerialDevice& other)
{
  // other's bytes go after ours, in order
  const std::size_t count = u8_deque_count(&other.m_tx_buffer);
  for (std::size_t i = 0; i < count; i++)
    u8_deque_push_back_growing(&m_tx_buffer, u8_deque_at(&other.m_tx_buffer, i));

  u8_deque_clear(&other.m_tx_buffer);
}

void SerialDevice::DoState(PointerWrap& p)
{
  p.Do(m_rx_buffer);
  DO_RETRO_DEQUE(p, u8_deque, &m_tx_buffer);
}

}  // namespace Triforce
