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

// The class a device is, which its calls branch on; Empty is the base itself, an empty slot
enum class EXIDeviceClass : u8
{
  Empty,
  Dummy,
  MemoryCard,
  IPL,
  AD16,
  Mic,
  Ethernet,
  Modem,
  Gecko,
  AGP,
  Baseboard,
};

class IEXIDevice
{
public:
  explicit IEXIDevice(Core::System& system, EXIDeviceClass device_class = EXIDeviceClass::Empty);
  ~IEXIDevice() = default;
  IEXIDevice(const IEXIDevice&) = delete;
  IEXIDevice& operator=(const IEXIDevice&) = delete;

  // These branch to the device this is.

  // Immediate copy functions
  void ImmWrite(u32 data, u32 size);
  u32 ImmRead(u32 size);
  void ImmReadWrite(u32& data, u32 size);

  // DMA copy functions
  void DMAWrite(u32 address, u32 size);
  void DMARead(u32 address, u32 size);

  bool UseDelayedTransferCompletion() const;
  bool IsPresent() const;
  void SetCS(int cs);
  void DoState(PointerWrap& p);

  // Is generating interrupt ?
  bool IsInterruptSet();

  const EXIDeviceClass device_class;

  // For savestates. storing it here seemed cleaner than requiring each implementation to report its
  // type. I know this class is set up like an interface, but no code requires it to be strictly
  // such.
  EXIDeviceType m_device_type = EXIDeviceType::None;

protected:
  // What a device that has no call of its own does; the copies go a byte at a time
  void ImmWriteDefault(u32 data, u32 size);
  u32 ImmReadDefault(u32 size);
  void ImmReadWriteDefault(u32&, u32) {}
  void DMAWriteDefault(u32 address, u32 size);
  void DMAReadDefault(u32 address, u32 size);
  bool UseDelayedTransferCompletionDefault() const { return false; }
  bool IsPresentDefault() const { return false; }
  void SetCSDefault(int) {}
  void DoStateDefault(PointerWrap&) {}
  bool IsInterruptSetDefault() { return false; }
  void TransferByteDefault(u8&) {}

  Core::System& m_system;

private:
  // Byte transfer function for this device
  void TransferByte(u8& byte);
};

// Made with new; freed with EXIDevice_Destroy as the device it is
IEXIDevice* EXIDevice_Create(Core::System& system, EXIDeviceType device_type, int channel_num,
                             const Memcard::HeaderData& memcard_header_data);
void EXIDevice_Destroy(IEXIDevice* device);
}  // namespace ExpansionInterface
