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
  DEBUG_ASSERT(m_tx_buffer.size() >= size);

  for (std::size_t i = 0; i < size; i++)
    bytes[i] = m_tx_buffer[i];
  m_tx_buffer.erase(m_tx_buffer.begin(), m_tx_buffer.begin() + size);
}

void SerialDevice::ConsumeRxBytes(std::size_t count)
{
  DEBUG_ASSERT(m_rx_buffer.size() >= count);

  m_rx_buffer.erase(m_rx_buffer.begin(), m_rx_buffer.begin() + count);
}

void SerialDevice::WriteTxBytes(const u8* bytes, std::size_t size)
{
  m_tx_buffer.insert(m_tx_buffer.end(), bytes, bytes + size);
}

void SerialDevice::PassThroughTxBytes(SerialDevice& other)
{
  // The destination buffer will often be empty or near-empty.
  if (m_tx_buffer.size() < other.m_tx_buffer.size()) [[likely]]
  {
    other.m_tx_buffer.insert(other.m_tx_buffer.begin(), m_tx_buffer.begin(), m_tx_buffer.end());
    m_tx_buffer.swap(other.m_tx_buffer);
  }
  else
  {
    m_tx_buffer.insert(m_tx_buffer.end(), other.m_tx_buffer.begin(), other.m_tx_buffer.end());
  }

  other.m_tx_buffer.clear();
}

void SerialDevice::DoState(PointerWrap& p)
{
  p.Do(m_rx_buffer);
  p.Do(m_tx_buffer);
}

}  // namespace Triforce
