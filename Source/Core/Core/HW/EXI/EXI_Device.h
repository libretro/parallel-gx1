// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace Memcard
{
struct HeaderData;
}

namespace ExpansionInterface
{
enum class EXIDeviceType : int
{
  Dummy,
  MemoryCard,
  MaskROM,
  AD16,
  Microphone,
  Ethernet,
  Baseboard,
  Gecko,
  // Only used when creating a device by EXIDevice_Create.
  // Converted to MemoryCard internally.
  MemoryCardFolder,
  AGP,
  EthernetXLink,
  EthernetTapServer,
  EthernetBuiltIn,
  ModemTapServer,
  EthernetIPC,
  None = 0xFF
};

class IEXIDevice
{
public:
  explicit IEXIDevice(Core::System& system);
  virtual ~IEXIDevice() = default;

  // Immediate copy functions
  virtual void ImmWrite(u32 data, u32 size);
  virtual u32 ImmRead(u32 size);
  virtual void ImmReadWrite(u32& data, u32 size);

  // DMA copy functions
  virtual void DMAWrite(u32 address, u32 size);
  virtual void DMARead(u32 address, u32 size);

  virtual bool UseDelayedTransferCompletion() const;
  virtual bool IsPresent() const;
  virtual void SetCS(int cs);
  virtual void DoState(PointerWrap& p);

  // Is generating interrupt ?
  virtual bool IsInterruptSet();

  // For savestates. storing it here seemed cleaner than requiring each implementation to report its
  // type. I know this class is set up like an interface, but no code requires it to be strictly
  // such.
  EXIDeviceType m_device_type = EXIDeviceType::None;

protected:
  Core::System& m_system;

private:
  // Byte transfer function for this device
  virtual void TransferByte(u8& byte);
};

std::unique_ptr<IEXIDevice> EXIDevice_Create(Core::System& system, EXIDeviceType device_type,
                                             int channel_num,
                                             const Memcard::HeaderData& memcard_header_data);
}  // namespace ExpansionInterface
