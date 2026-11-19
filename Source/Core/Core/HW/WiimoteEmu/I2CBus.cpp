// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteEmu/I2CBus.h"

namespace WiimoteEmu
{
void I2CBus::AddSlave(I2CSlave* slave)
{
  if (m_slave_count < MAX_SLAVES)
    m_slaves[m_slave_count++] = slave;
}

void I2CBus::RemoveSlave(I2CSlave* slave)
{
  int kept = 0;
  for (int i = 0; i < m_slave_count; i++)
  {
    if (m_slaves[i] != slave)
      m_slaves[kept++] = m_slaves[i];
  }
  m_slave_count = kept;
}

void I2CBus::Reset()
{
  m_slave_count = 0;
}

int I2CBus::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  for (int i = 0; i < m_slave_count; i++)
  {
    const int bytes_read = m_slaves[i]->BusRead(slave_addr, addr, count, data_out);

    // A slave responded, we are done.
    if (bytes_read)
      return bytes_read;
  }

  return 0;
}

int I2CBus::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  for (int i = 0; i < m_slave_count; i++)
  {
    const int bytes_written = m_slaves[i]->BusWrite(slave_addr, addr, count, data_in);

    // A slave responded, we are done.
    if (bytes_written)
      return bytes_written;
  }

  return 0;
}

}  // namespace WiimoteEmu
