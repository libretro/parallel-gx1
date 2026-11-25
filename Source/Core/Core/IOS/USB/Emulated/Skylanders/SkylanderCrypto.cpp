// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Emulated/Skylanders/SkylanderCrypto.h"

#include <retro_miscellaneous.h>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"

namespace IOS::HLE::USB::SkylanderCrypto
{
u16 ComputeCRC16(const u8* data, size_t size)
{
  const u16 polynomial = 0x1021;

  u16 crc = 0xFFFF;

  for (size_t i = 0; i < size; ++i)
  {
    crc ^= data[i] << 8;

    for (size_t j = 0; j < 8; j++)
    {
      if (Common::ExtractBit(crc, 15))
      {
        crc = (crc << 1) ^ polynomial;
      }
      else
      {
        crc <<= 1;
      }
    }
  }

  return crc;
}
void ComputeChecksumType1(const u8* data_start, u8* output)
{
  u8 input[0x10] = {};
  memcpy(input, data_start, 0x10);
  input[0xE] = 0x05;
  input[0xF] = 0x00;
  u16 crc = ComputeCRC16(input, ARRAY_SIZE(input));
  memcpy(output, &crc, 2);
}
void ComputeChecksumType2(const u8* data_start, u8* output)
{
  u8 input[0x30] = {};
  memcpy(input, data_start, 0x20);
  memcpy(input + 0x20, data_start + 0x30, 0x10);
  u16 crc = ComputeCRC16(input, ARRAY_SIZE(input));
  memcpy(output, &crc, 2);
}
void ComputeChecksumType3(const u8* data_start, u8* output)
{
  u8 input[0x110] = {};
  memcpy(input, data_start, 0x20);
  memcpy(input + 0x20, data_start + 0x30, 0x10);
  u16 crc = ComputeCRC16(input, ARRAY_SIZE(input));
  memcpy(output, &crc, 2);
}

void ComputeChecksumType6(const u8* data_start, u8* output)
{
  u8 input[0x40] = {};
  memcpy(input, data_start, 0x20);
  memcpy(input + 0x20, data_start + 0x30, 0x20);

  input[0x0] = 0x06;
  input[0x1] = 0x01;

  u16 crc = ComputeCRC16(input, ARRAY_SIZE(input));
  memcpy(output, &crc, 2);
}
void ComputeToyCode(u64 code, u8* out)
{
  if (code == 0)
  {
    static constexpr u8 invalid_code_result[11]{static_cast<u8>('N'), static_cast<u8>('/'),
                                                static_cast<u8>('A')};
    std::memcpy(out, invalid_code_result, sizeof(invalid_code_result));
    return;
  }

  u8 code_bytes[10];
  for (size_t i = 0; i < ARRAY_SIZE(code_bytes); ++i)
  {
    code_bytes[i] = static_cast<u8>(code % 29);
    code /= 29;
  }

  static constexpr char lookup_table[] = "23456789BCDFGHJKLMNPQRSTVWXYZ";
  u8 code_chars[10];
  for (size_t i = 0; i < ARRAY_SIZE(code_bytes); ++i)
  {
    code_chars[i] = static_cast<u8>(lookup_table[code_bytes[9 - i]]);
  }

  std::memcpy(&out[0], &code_chars[0], 5);
  out[5] = static_cast<u8>('-');
  std::memcpy(&out[6], &code_chars[5], 5);
}
}  // namespace IOS::HLE::USB::SkylanderCrypto
