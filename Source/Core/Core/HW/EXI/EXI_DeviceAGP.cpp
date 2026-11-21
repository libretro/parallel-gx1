// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceAGP.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/EXI/EXI.h"

namespace ExpansionInterface
{
CEXIAgp::CEXIAgp(Core::System& system, Slot slot) : IEXIDevice(system, EXIDeviceClass::AGP)
{
  ASSERT(IsMemcardSlot(slot));
  m_slot = slot;

  // Create the ROM
  m_rom_size = 0;

  LoadRom();

  m_address = 0;
}

CEXIAgp::~CEXIAgp()
{
  // Room is left for the ".sav" put after the base path.
  char path[PATH_MAX_LENGTH];
  GetCartBasePath(path, sizeof(path) - 4);

  if (path[0])
  {
    strlcat(path, ".sav", sizeof(path));
    SaveFileFromEEPROM(path);
  }
  std::free(m_rom);
  std::free(m_eeprom);
}

void CEXIAgp::GetCartBasePath(char* out, size_t out_size) const
{
  strlcpy(out, Config::Get(Config::GetInfoForAGPCartPath(m_slot)).c_str(), out_size);
  path_remove_extension(out);
}

// Makes a buffer of size bytes, zeroed, in place of the one at *buffer.
static void ResizeBuffer(u8** buffer, u32* buffer_size, u32 size)
{
  std::free(*buffer);
  *buffer = size ? static_cast<u8*>(std::calloc(size, 1)) : nullptr;
  *buffer_size = *buffer ? size : 0;
}

// Saves a buffer as a vector of bytes was: a u32 count, then the bytes.
static void DoBuffer(PointerWrap& p, u8** buffer, u32* buffer_size)
{
  u32 size = *buffer_size;
  p.Do(size);
  if (p.IsReadMode() && size != *buffer_size)
    ResizeBuffer(buffer, buffer_size, size);
  if (size != *buffer_size)
  {
    p.SetMeasureMode();
    return;
  }
  p.DoArray(*buffer, size);
}

void CEXIAgp::CRC8(const u8* data, u32 size)
{
  for (u32 it = 0; it < size; it++)
  {
    u8 crc = 0;
    m_hash = m_hash ^ data[it];
    if (m_hash & 1)
      crc ^= 0x5e;
    if (m_hash & 2)
      crc ^= 0xbc;
    if (m_hash & 4)
      crc ^= 0x61;
    if (m_hash & 8)
      crc ^= 0xc2;
    if (m_hash & 0x10)
      crc ^= 0x9d;
    if (m_hash & 0x20)
      crc ^= 0x23;
    if (m_hash & 0x40)
      crc ^= 0x46;
    if (m_hash & 0x80)
      crc ^= 0x8c;
    m_hash = crc;
  }
}

void CEXIAgp::LoadRom()
{
  // Load whole ROM dump
  // Room is left for the ".sav" put after the base path.
  char path[PATH_MAX_LENGTH];
  GetCartBasePath(path, sizeof(path) - 4);
  LoadFileToROM(Config::Get(Config::GetInfoForAGPCartPath(m_slot)).c_str());
  INFO_LOG(EXPANSIONINTERFACE, "Loaded GBA rom: %s card: %u", path, static_cast<unsigned>(m_slot));
  const size_t base_length = std::strlen(path);
  strlcat(path, ".sav", sizeof(path));
  LoadFileToEEPROM(path);
  path[base_length] = '\0';
  INFO_LOG(EXPANSIONINTERFACE, "Loaded GBA sav: %s card: %u", path, static_cast<unsigned>(m_slot));
}

void CEXIAgp::LoadFileToROM(const char* filename)
{
  File::IOFile pStream(filename, "rb");
  if (pStream)
  {
    u64 filesize = pStream.GetSize();
    m_rom_size = filesize & 0xFFFFFFFF;
    m_rom_mask = (m_rom_size - 1);

    ResizeBuffer(&m_rom, &m_rom_buffer_size, m_rom_size);

    pStream.ReadBytes(m_rom, m_rom_buffer_size);
  }
  else
  {
    // dummy rom data
    ResizeBuffer(&m_rom, &m_rom_buffer_size, 0x2000);
  }
}

void CEXIAgp::LoadFileToEEPROM(const char* filename)
{
  // Technically one of EEPROM, Flash, SRAM, FRAM
  File::IOFile pStream(filename, "rb");
  if (pStream)
  {
    u64 filesize = pStream.GetSize();
    m_eeprom_size = filesize & 0xFFFFFFFF;
    m_eeprom_mask = (m_eeprom_size - 1);

    ResizeBuffer(&m_eeprom, &m_eeprom_buffer_size, m_eeprom_size);
    pStream.ReadBytes(m_eeprom, m_eeprom_buffer_size);
    if ((m_eeprom_size == 512) || (m_eeprom_size == 8192))
    {
      // Handle endian read - could be done with byte access in 0xAE commands instead
      for (u32 index = 0; index < (m_eeprom_size / 8); index++)
      {
        u64 NewVal = 0;
        for (u32 indexb = 0; indexb < 8; indexb++)
          NewVal = (NewVal << 0x8) | m_eeprom[index * 8 + indexb];
        ((u64*)m_eeprom)[index] = NewVal;
      }
      m_eeprom_add_end = (m_eeprom_size == 512 ? (2 + 6) : (2 + 14));
      m_eeprom_add_mask = (m_eeprom_size == 512 ? 0x3F : 0x3FF);
      m_eeprom_read_mask = (m_eeprom_size == 512 ? 0x80 : 0x8000);
      m_eeprom_status_mask = (m_rom_size == 0x2000000 ? 0x1FFFF00 : 0x1000000);
    }
    else
      m_eeprom_status_mask = 0;
  }
  else
  {
    m_eeprom_size = 0;
    ResizeBuffer(&m_eeprom, &m_eeprom_buffer_size, 0);
  }
}

void CEXIAgp::SaveFileFromEEPROM(const char* filename)
{
  File::IOFile pStream(filename, "wb");
  if (pStream)
  {
    if ((m_eeprom_size == 512) || (m_eeprom_size == 8192))
    {
      // Handle endian write - could be done with byte access in 0xAE commands instead
      // Each word is written big-endian over itself, then put back.
      for (u32 index = 0; index < (m_eeprom_size / 8); index++)
      {
        const u64 NewVal = ((u64*)m_eeprom)[index];
        for (u32 indexb = 0; indexb < 8; indexb++)
          m_eeprom[index * 8 + (7 - indexb)] = (NewVal >> (indexb * 8)) & 0xFF;
      }
      pStream.WriteBytes(m_eeprom, m_eeprom_size);
      for (u32 index = 0; index < (m_eeprom_size / 8); index++)
      {
        u64 NewVal = 0;
        for (u32 indexb = 0; indexb < 8; indexb++)
          NewVal = (NewVal << 0x8) | m_eeprom[index * 8 + indexb];
        ((u64*)m_eeprom)[index] = NewVal;
      }
    }
    else
    {
      pStream.WriteBytes(m_eeprom, m_eeprom_size);
    }
  }
}

u32 CEXIAgp::ImmRead(u32 _uSize)
{
  u32 uData = 0;
  u8 RomVal1, RomVal2, RomVal3, RomVal4;

  switch (m_current_cmd)
  {
  case 0xAE000000:       // Clock handshake?
    uData = 0x5AAA5517;  // 17 is precalculated hash
    m_current_cmd = 0;
    break;
  case 0xAE010000:  // Init?
    uData = (m_return_pos == 0) ? 0x01020304 :
                                  0xF0020304;  // F0 is precalculated hash, 020304 is left over
    if (m_return_pos == 1)
      m_current_cmd = 0;
    else
      m_return_pos = 1;
    break;
  case 0xAE020000:  // Read 2 bytes with 24 bit address
    if (m_eeprom_write_status && ((m_rw_offset & m_eeprom_status_mask) == m_eeprom_status_mask) &&
        (m_eeprom_status_mask != 0))
    {
      RomVal1 = 0x1;
      RomVal2 = 0x0;
    }
    else
    {
      RomVal1 = m_rom[(m_rw_offset++) & m_rom_mask];
      RomVal2 = m_rom[(m_rw_offset++) & m_rom_mask];
    }
    CRC8(&RomVal2, 1);
    CRC8(&RomVal1, 1);
    uData = (RomVal2 << 24) | (RomVal1 << 16) | (m_hash << 8);
    m_current_cmd = 0;
    break;
  case 0xAE030000:  // read the next 4 bytes out of 0x10000 group
    if (_uSize == 1)
    {
      uData = 0xFF000000;
      m_current_cmd = 0;
    }
    else
    {
      RomVal1 = m_rom[(m_rw_offset++) & m_rom_mask];
      RomVal2 = m_rom[(m_rw_offset++) & m_rom_mask];
      RomVal3 = m_rom[(m_rw_offset++) & m_rom_mask];
      RomVal4 = m_rom[(m_rw_offset++) & m_rom_mask];
      CRC8(&RomVal2, 1);
      CRC8(&RomVal1, 1);
      CRC8(&RomVal4, 1);
      CRC8(&RomVal3, 1);
      uData = (RomVal2 << 24) | (RomVal1 << 16) | (RomVal4 << 8) | (RomVal3);
    }
    break;
  case 0xAE040000:  // read 1 byte from 16 bit address
    // ToDo: Flash special handling
    if (m_eeprom_size == 0)
      RomVal1 = 0xFF;
    else
      RomVal1 = m_eeprom[m_eeprom_pos];
    CRC8(&RomVal1, 1);
    uData = (RomVal1 << 24) | (m_hash << 16);
    m_current_cmd = 0;
    break;
  case 0xAE0B0000:  // read 1 bit from DMA with 6 or 14 bit address
    // Change to byte access instead of endian file access?
    RomVal1 = EE_READ_FALSE;
    if ((m_eeprom_size != 0) && (m_eeprom_pos >= EE_IGNORE_BITS) &&
        ((((u64*)m_eeprom)[(m_eeprom_cmd >> 1) & m_eeprom_add_mask]) >>
         ((EE_DATA_BITS - 1) - (m_eeprom_pos - EE_IGNORE_BITS))) &
            0x1)
    {
      RomVal1 = EE_READ_TRUE;
    }
    RomVal2 = 0;
    CRC8(&RomVal2, 1);
    CRC8(&RomVal1, 1);
    uData = (RomVal2 << 24) | (RomVal1 << 16) | (m_hash << 8);
    m_eeprom_pos++;
    m_current_cmd = 0;
    break;
  case 0xAE070000:  // complete write 1 byte from 16 bit address
  case 0xAE0C0000:  // complete write 1 bit from dma with 6 or 14 bit address
    uData = m_hash << 24;
    m_current_cmd = 0;
    break;
  default:
    uData = 0x0;
    m_current_cmd = 0;
    break;
  }
  DEBUG_LOG(EXPANSIONINTERFACE, "AGP read %x", uData);
  return uData;
}

void CEXIAgp::ImmWrite(u32 _uData, u32 _uSize)
{
  // 0x00 = Execute current command?
  if ((_uSize == 1) && ((_uData & 0xFF000000) == 0))
    return;

  u8 HashCmd;
  u64 Mask;
  DEBUG_LOG(EXPANSIONINTERFACE, "AGP command %x", _uData);
  switch (m_current_cmd)
  {
  case 0xAE020000:  // set up 24 bit address for read 2 bytes
  case 0xAE030000:  // set up 24 bit address for read (0x10000 byte group)
    // 25 bit address shifted one bit right = 24 bits
    m_rw_offset = ((_uData & 0xFFFFFF00) >> (8 - 1));
    m_return_pos = 0;
    HashCmd = (_uData & 0xFF000000) >> 24;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x00FF0000) >> 16;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x0000FF00) >> 8;
    CRC8(&HashCmd, 1);
    break;
  case 0xAE040000:  // set up 16 bit address for read 1 byte
    // ToDo: Flash special handling
    m_eeprom_pos = ((_uData & 0xFFFF0000) >> 0x10) & m_eeprom_mask;
    HashCmd = (_uData & 0xFF000000) >> 24;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x00FF0000) >> 16;
    CRC8(&HashCmd, 1);
    break;
  case 0xAE070000:  // write 1 byte from 16 bit address
    // ToDo: Flash special handling
    m_eeprom_pos = ((_uData & 0xFFFF0000) >> 0x10) & m_eeprom_mask;
    if (m_eeprom_size != 0)
      (m_eeprom)[(m_eeprom_pos)] = (_uData & 0x0000FF00) >> 0x8;
    HashCmd = (_uData & 0xFF000000) >> 24;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x00FF0000) >> 16;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x0000FF00) >> 8;
    CRC8(&HashCmd, 1);
    break;
  case 0xAE0C0000:  // write 1 bit from dma with 6 or 14 bit address
    if ((m_eeprom_pos < m_eeprom_add_end) ||
        (m_eeprom_pos == ((m_eeprom_cmd & m_eeprom_read_mask) ? m_eeprom_add_end :
                                                                m_eeprom_add_end + EE_DATA_BITS)))
    {
      Mask = (1ULL << (m_eeprom_add_end - std::min(m_eeprom_pos, m_eeprom_add_end)));
      if ((_uData >> 16) & 0x1)
        m_eeprom_cmd |= Mask;
      else
        m_eeprom_cmd &= ~Mask;
      if (m_eeprom_pos == m_eeprom_add_end + EE_DATA_BITS)
      {
        // Change to byte access instead of endian file access?
        if (m_eeprom_size != 0)
          ((u64*)m_eeprom)[(m_eeprom_cmd >> 1) & m_eeprom_add_mask] = m_eeprom_data;
        m_eeprom_write_status = true;
      }
    }
    else
    {
      Mask = (1ULL << (m_eeprom_add_end + EE_DATA_BITS - 1 - m_eeprom_pos));
      if ((_uData >> 16) & 0x1)
        m_eeprom_data |= Mask;
      else
        m_eeprom_data &= ~Mask;
    }
    m_eeprom_pos++;
    m_return_pos = 0;
    HashCmd = (_uData & 0xFF000000) >> 24;
    CRC8(&HashCmd, 1);
    HashCmd = (_uData & 0x00FF0000) >> 16;
    CRC8(&HashCmd, 1);
    break;
  case 0xAE0B0000:
    m_eeprom_write_status = false;
    break;
  case 0xAE000000:
  case 0xAE010000:
  case 0xAE090000:                  // start DMA
    m_eeprom_write_status = false;  // ToDo: Verify with hardware which commands disable EEPROM CS
    [[fallthrough]];
  case 0xAE0A0000:  // end DMA
    m_eeprom_pos = 0;
    [[fallthrough]];
  default:
    m_current_cmd = _uData;
    m_return_pos = 0;
    m_hash = 0xFF;
    HashCmd = (_uData & 0x00FF0000) >> 16;
    CRC8(&HashCmd, 1);
    break;
  }
}

void CEXIAgp::DoState(PointerWrap& p)
{
  p.Do(m_slot);
  p.Do(m_address);
  p.Do(m_current_cmd);
  DoBuffer(p, &m_eeprom, &m_eeprom_buffer_size);
  p.Do(m_eeprom_cmd);
  p.Do(m_eeprom_data);
  p.Do(m_eeprom_mask);
  p.Do(m_eeprom_pos);
  p.Do(m_eeprom_size);
  p.Do(m_eeprom_add_end);
  p.Do(m_eeprom_add_mask);
  p.Do(m_eeprom_read_mask);
  p.Do(m_eeprom_status_mask);
  p.Do(m_eeprom_write_status);
  p.Do(m_hash);
  p.Do(m_position);
  p.Do(m_return_pos);
  DoBuffer(p, &m_rom, &m_rom_buffer_size);
  p.Do(m_rom_mask);
  p.Do(m_rom_size);
  p.Do(m_rw_offset);
}
}  // namespace ExpansionInterface
