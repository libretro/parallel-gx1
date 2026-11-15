// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_Device.h"

#include <type_traits>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_DeviceAD16.h"
#include "Core/HW/EXI/EXI_DeviceAGP.h"
#include "Core/HW/EXI/EXI_DeviceBaseboard.h"
#include "Core/HW/EXI/EXI_DeviceDummy.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"
#include "Core/HW/EXI/EXI_DeviceGecko.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/EXI/EXI_DeviceModem.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "Core/HW/EXI/EXI_DeviceMic.h"

namespace ExpansionInterface
{
IEXIDevice::IEXIDevice(Core::System& system, EXIDeviceClass device_class_)
    : device_class(device_class_), m_system(system)
{
}

void IEXIDevice::ImmWriteDefault(u32 data, u32 size)
{
  while (size--)
  {
    u8 byte = data >> 24;
    TransferByte(byte);
    data <<= 8;
  }
}

u32 IEXIDevice::ImmReadDefault(u32 size)
{
  u32 result = 0;
  u32 position = 0;
  while (size--)
  {
    u8 byte = 0;
    TransferByte(byte);
    result |= byte << (24 - (position++ * 8));
  }
  return result;
}

void IEXIDevice::DMAWriteDefault(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  while (size--)
  {
    u8 byte = memory.Read_U8(address++);
    TransferByte(byte);
  }
}

void IEXIDevice::DMAReadDefault(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  while (size--)
  {
    u8 byte = 0;
    TransferByte(byte);
    memory.Write_U8(byte, address++);
  }
}

/* The call on the device class this is: its own or, when it has none, the base's default
 * (named Fn##Default). An empty slot is the base itself and takes every default. */
#define EXI_CASE(kind, D, Q)                                                                       \
  case EXIDeviceClass::kind:                                                                       \
    return call(static_cast<Q D*>(this));

#define EXI_DISPATCH(Q)                                                                            \
  switch (device_class)                                                                            \
  {                                                                                                \
    EXI_CASE(Dummy, CEXIDummy, Q)                                                                  \
    EXI_CASE(MemoryCard, CEXIMemoryCard, Q)                                                        \
    EXI_CASE(IPL, CEXIIPL, Q)                                                                      \
    EXI_CASE(AD16, CEXIAD16, Q)                                                                    \
    EXI_CASE(Mic, CEXIMic, Q)                                                                      \
    EXI_CASE(Ethernet, CEXIETHERNET, Q)                                                            \
    EXI_CASE(Modem, CEXIModem, Q)                                                                  \
    EXI_CASE(Gecko, CEXIGecko, Q)                                                                  \
    EXI_CASE(AGP, CEXIAgp, Q)                                                                      \
    EXI_CASE(Baseboard, CEXIBaseboard, Q)                                                          \
  case EXIDeviceClass::Empty:                                                                      \
  default:                                                                                         \
    break;                                                                                         \
  }                                                                                                \
  return call(this);

// Whether D has a call of its own, and not the base's
#define EXI_HAS_OWN(D, Fn) (!std::is_same_v<decltype(&D::Fn), decltype(&IEXIDevice::Fn)>)

#define EXI_CALL(Fn, Q, ...)                                                                       \
  const auto call = [&](Q auto* device) {                                                          \
    using D = std::remove_cv_t<std::remove_pointer_t<decltype(device)>>;                           \
    if constexpr (EXI_HAS_OWN(D, Fn))                                                              \
      return device->Fn(__VA_ARGS__);                                                              \
    else                                                                                           \
      return Fn##Default(__VA_ARGS__);                                                             \
  };                                                                                               \
  EXI_DISPATCH(Q)

void IEXIDevice::ImmWrite(u32 data, u32 size){EXI_CALL(ImmWrite, , data, size)}

u32 IEXIDevice::ImmRead(u32 size)
{
  EXI_CALL(ImmRead, , size)
}

void IEXIDevice::ImmReadWrite(u32& data, u32 size)
{
  EXI_CALL(ImmReadWrite, , data, size)
}

void IEXIDevice::DMAWrite(u32 address, u32 size)
{
  EXI_CALL(DMAWrite, , address, size)
}

void IEXIDevice::DMARead(u32 address, u32 size)
{
  EXI_CALL(DMARead, , address, size)
}

bool IEXIDevice::UseDelayedTransferCompletion() const
{
  EXI_CALL(UseDelayedTransferCompletion, const)
}

bool IEXIDevice::IsPresent() const
{
  EXI_CALL(IsPresent, const)
}

void IEXIDevice::SetCS(int cs)
{
  EXI_CALL(SetCS, , cs)
}

void IEXIDevice::DoState(PointerWrap& p)
{
  EXI_CALL(DoState, , p)
}

bool IEXIDevice::IsInterruptSet()
{
  EXI_CALL(IsInterruptSet, )
}

void IEXIDevice::TransferByte(u8& byte){EXI_CALL(TransferByte, , byte)}

// F A C T O R Y
IEXIDevice* EXIDevice_Create(Core::System& system, const EXIDeviceType device_type,
                             const int channel_num, const Memcard::HeaderData& memcard_header_data)
{
  IEXIDevice* result;
  // XXX This computation isn't necessarily right (it holds for A/B, but not SP1)
  // However, the devices that care about slots currently only go in A/B.
  const Slot slot = static_cast<Slot>(channel_num);

  switch (device_type)
  {
  case EXIDeviceType::Dummy:
    result = new CEXIDummy(system, "Dummy");
    break;

  case EXIDeviceType::MemoryCard:
  case EXIDeviceType::MemoryCardFolder:
  {
    bool gci_folder = (device_type == EXIDeviceType::MemoryCardFolder);
    result = new CEXIMemoryCard(system, slot, gci_folder, memcard_header_data);
    break;
  }
  case EXIDeviceType::MaskROM:
    result = new CEXIIPL(system);
    break;

  case EXIDeviceType::AD16:
    result = new CEXIAD16(system);
    break;

  case EXIDeviceType::Microphone:
    result = new CEXIMic(system, channel_num);
    break;

  case EXIDeviceType::Ethernet:
    result = new CEXIETHERNET(system, BBADeviceType::TAP);
    break;

  case EXIDeviceType::EthernetTapServer:
    result = new CEXIETHERNET(system, BBADeviceType::TAPSERVER);
    break;

  case EXIDeviceType::EthernetXLink:
    result = new CEXIETHERNET(system, BBADeviceType::XLINK);
    break;

  case EXIDeviceType::EthernetBuiltIn:
    result = new CEXIETHERNET(system, BBADeviceType::BuiltIn);
    break;

  case EXIDeviceType::EthernetIPC:
    result = new CEXIETHERNET(system, BBADeviceType::IPC);
    break;

  case EXIDeviceType::ModemTapServer:
    result = new CEXIModem(system, ModemDeviceType::TAPSERVER);
    break;

  case EXIDeviceType::Gecko:
    result = new CEXIGecko(system);
    break;

  case EXIDeviceType::AGP:
    result = new CEXIAgp(system, slot);
    break;

  case EXIDeviceType::Baseboard:
    result = new CEXIBaseboard(system);
    break;

  case EXIDeviceType::None:
  default:
    result = new IEXIDevice(system);
    break;
  }

  if (result != nullptr)
    result->m_device_type = device_type;

  return result;
}
void EXIDevice_Destroy(IEXIDevice* device)
{
  if (!device)
    return;
  const auto call = [](auto* d) { delete d; };
  switch (device->device_class)
  {
  case EXIDeviceClass::Dummy:
    return call(static_cast<CEXIDummy*>(device));
  case EXIDeviceClass::MemoryCard:
    return call(static_cast<CEXIMemoryCard*>(device));
  case EXIDeviceClass::IPL:
    return call(static_cast<CEXIIPL*>(device));
  case EXIDeviceClass::AD16:
    return call(static_cast<CEXIAD16*>(device));
  case EXIDeviceClass::Mic:
    return call(static_cast<CEXIMic*>(device));
  case EXIDeviceClass::Ethernet:
    return call(static_cast<CEXIETHERNET*>(device));
  case EXIDeviceClass::Modem:
    return call(static_cast<CEXIModem*>(device));
  case EXIDeviceClass::Gecko:
    return call(static_cast<CEXIGecko*>(device));
  case EXIDeviceClass::AGP:
    return call(static_cast<CEXIAgp*>(device));
  case EXIDeviceClass::Baseboard:
    return call(static_cast<CEXIBaseboard*>(device));
  case EXIDeviceClass::Empty:
  default:
    return call(device);
  }
}
}  // namespace ExpansionInterface
