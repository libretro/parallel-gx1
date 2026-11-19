// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>

#include "Common/CommonTypes.h"

namespace WiimoteEmu
{
class I2CBus;

class I2CSlave
{
  friend I2CBus;

protected:
  virtual ~I2CSlave() = default;

  virtual int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) = 0;
  virtual int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) = 0;

  // Every slave's register block is 0x100 bytes.
  static constexpr int REGISTER_SIZE = 0x100;

  static int RawRead(void* reg_data, u8 addr, int count, u8* data_out)
  {
    // TODO: addr wraps around after 0xff

    const u8* src = static_cast<const u8*>(reg_data) + addr;
    const int available = REGISTER_SIZE - addr;
    if (count > available)
      count = available;

    memcpy(data_out, src, count);

    return count;
  }

  static int RawWrite(void* reg_data, u8 addr, int count, const u8* data_in)
  {
    // TODO: addr wraps around after 0xff

    u8* dst = static_cast<u8*>(reg_data) + addr;
    const int available = REGISTER_SIZE - addr;
    if (count > available)
      count = available;

    memcpy(dst, data_in, count);

    return count;
  }
};

class I2CBus
{
public:
  void AddSlave(I2CSlave* slave);
  void RemoveSlave(I2CSlave* slave);

  void Reset();

  // TODO: change int to u16 or something
  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out);
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in);

private:
  // The Wii Remote's bus holds the speaker, the camera and the extension.
  static constexpr int MAX_SLAVES = 4;

  // Pointers are unowned:
  I2CSlave* m_slaves[MAX_SLAVES] = {};
  int m_slave_count = 0;
};

}  // namespace WiimoteEmu
