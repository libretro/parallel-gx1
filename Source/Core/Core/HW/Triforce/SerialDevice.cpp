// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Triforce/SerialDevice.h"

#include <cstdlib>
#include <cstring>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"

namespace Triforce
{

SerialDevice::SerialDevice()
{
  u8_deque_init(&m_tx_buffer, 256);
  m_rx_buffer = static_cast<u8*>(malloc(1024));
  m_rx_capacity = m_rx_buffer ? 1024 : 0;
}

SerialDevice::~SerialDevice()
{
  free(m_rx_buffer);
  u8_deque_free(&m_tx_buffer);
}

void SerialDevice::WriteRxBytes(const u8* bytes, std::size_t size)
{
  if (m_rx_end + size > m_rx_capacity)
  {
    const std::size_t pending = m_rx_end - m_rx_start;
    memmove(m_rx_buffer, m_rx_buffer + m_rx_start, pending);
    m_rx_start = 0;
    m_rx_end = pending;

    if (pending + size > m_rx_capacity)
    {
      std::size_t capacity = m_rx_capacity ? m_rx_capacity * 2 : 1024;
      while (capacity < pending + size)
        capacity *= 2;
      u8* const buffer = static_cast<u8*>(realloc(m_rx_buffer, capacity));
      if (!buffer)
        return;
      m_rx_buffer = buffer;
      m_rx_capacity = capacity;
    }
  }

  memcpy(m_rx_buffer + m_rx_end, bytes, size);
  m_rx_end += size;
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
  DEBUG_ASSERT(GetRxByteCount() >= count);

  m_rx_start += count;
  if (m_rx_start == m_rx_end)
    m_rx_start = m_rx_end = 0;
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
  // Same layout as the std::vector this replaced: the byte count, then the bytes.
  u32 rx_count = u32(GetRxByteCount());
  p.Do(rx_count);
  if (p.IsReadMode())
  {
    m_rx_start = m_rx_end = 0;
    if (rx_count > m_rx_capacity)
    {
      u8* const buffer = static_cast<u8*>(realloc(m_rx_buffer, rx_count));
      if (!buffer)
      {
        p.SetMeasureMode();
        return;
      }
      m_rx_buffer = buffer;
      m_rx_capacity = rx_count;
    }
    m_rx_end = rx_count;
  }
  p.DoArray(m_rx_buffer + m_rx_start, rx_count);
  DO_RETRO_DEQUE(p, u8_deque, &m_tx_buffer);
}

}  // namespace Triforce
