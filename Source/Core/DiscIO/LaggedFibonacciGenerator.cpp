// SPDX-License-Identifier: CC0-1.0

#include "DiscIO/LaggedFibonacciGenerator.h"

#include <cstddef>
#include <cstring>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace DiscIO
{
void LaggedFibonacciGenerator::SetSeed(const u32 seed[SEED_SIZE])
{
  SetSeed(reinterpret_cast<const u8*>(seed));
}

void LaggedFibonacciGenerator::SetSeed(const u8 seed[SEED_SIZE * sizeof(u32)])
{
  m_position_bytes = 0;

  for (size_t i = 0; i < SEED_SIZE; ++i)
    m_buffer[i] = Common::swap32(seed + i * sizeof(u32));

  Initialize(false);
}

void LaggedFibonacciGenerator::GetBytes(size_t count, u8* out)
{
  while (count > 0)
  {
    const size_t buffer_left = LFG_K * sizeof(u32) - m_position_bytes;
    const size_t length = count < buffer_left ? count : buffer_left;

    std::memcpy(out, reinterpret_cast<u8*>(m_buffer.data()) + m_position_bytes, length);

    m_position_bytes += length;
    count -= length;
    out += length;

    if (m_position_bytes == LFG_K * sizeof(u32))
    {
      Forward();
      m_position_bytes = 0;
    }
  }
}

u8 LaggedFibonacciGenerator::GetByte()
{
  const u8 result = reinterpret_cast<u8*>(m_buffer.data())[m_position_bytes];

  ++m_position_bytes;

  if (m_position_bytes == LFG_K * sizeof(u32))
  {
    Forward();
    m_position_bytes = 0;
  }

  return result;
}

void LaggedFibonacciGenerator::Forward(size_t count)
{
  m_position_bytes += count;
  while (m_position_bytes >= LFG_K * sizeof(u32))
  {
    Forward();
    m_position_bytes -= LFG_K * sizeof(u32);
  }
}

void LaggedFibonacciGenerator::Forward()
{
  for (size_t i = 0; i < LFG_J; ++i)
    m_buffer[i] ^= m_buffer[i + LFG_K - LFG_J];

  for (size_t i = LFG_J; i < LFG_K; ++i)
    m_buffer[i] ^= m_buffer[i - LFG_J];
}

void LaggedFibonacciGenerator::Backward(size_t start_word, size_t end_word)
{
  const size_t loop_end = LFG_J > start_word ? LFG_J : start_word;
  for (size_t i = end_word < LFG_K ? end_word : LFG_K; i > loop_end; --i)
    m_buffer[i - 1] ^= m_buffer[i - 1 - LFG_J];

  for (size_t i = end_word < LFG_J ? end_word : LFG_J; i > start_word; --i)
    m_buffer[i - 1] ^= m_buffer[i - 1 + LFG_K - LFG_J];
}

bool LaggedFibonacciGenerator::Initialize(bool check_existing_data)
{
  for (size_t i = SEED_SIZE; i < LFG_K; ++i)
  {
    const u32 calculated = (m_buffer[i - 17] << 23) ^ (m_buffer[i - 16] >> 9) ^ m_buffer[i - 1];

    if (check_existing_data)
    {
      const u32 actual = (m_buffer[i] & 0xFF00FFFF) | (m_buffer[i] << 2 & 0x00FC0000);
      if ((calculated & 0xFFFCFFFF) != actual)
        return false;
    }

    m_buffer[i] = calculated;
  }

  // Instead of doing the "shift by 18 instead of 16" oddity when actually outputting the data,
  // we can do the shifting (and byteswapping) at this point to make the output code simpler.
  for (u32& x : m_buffer)
    x = Common::swap32((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000));

  for (size_t i = 0; i < 4; ++i)
    Forward();

  return true;
}

}  // namespace DiscIO
