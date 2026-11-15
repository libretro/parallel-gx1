// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/SI/SI_Device.h"

#include <string>
#include <type_traits>

#include <cstdio>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/HW/SI/SI_DeviceDanceMat.h"
#include "Core/HW/SI/SI_DeviceGBA.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/SI/SI_DeviceGBAEmu.h"
#endif
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceGCAdapter.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SI/SI_DeviceGCSteeringWheel.h"
#include "Core/HW/SI/SI_DeviceKeyboard.h"
#include "Core/HW/SI/SI_DeviceNull.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace SerialInterface
{
constexpr u64 GC_BITS_PER_SECOND = 200000;
constexpr u64 GBA_BITS_PER_SECOND = 250000;
constexpr u64 GC_STOP_BIT_NS = 6500;
constexpr u64 GBA_STOP_BIT_NS = 14000;

ISIDevice::ISIDevice(Core::System& system, SIDeviceClass device_class_, SIDevices device_type,
                     int device_number)
    : device_class(device_class_), m_system(system), m_device_number(device_number),
      m_device_type(device_type)
{
}

ISIDevice::~ISIDevice() = default;

int ISIDevice::GetDeviceNumber() const
{
  return m_device_number;
}

SIDevices ISIDevice::GetDeviceType() const
{
  return m_device_type;
}

int ISIDevice::RunBufferDefault(u8* buffer, int request_length)
{
#ifdef _DEBUG
  DEBUG_LOG(SERIALINTERFACE, "Send Data Device(%d) - Length(%d)   ", m_device_number,
            static_cast<int>(request_length));

  // eight bytes a line, as "0x.. "
  char line[8 * 5 + 1];
  int num = 0;
  while (num < request_length)
  {
    snprintf(line + (num % 8) * 5, sizeof(line) - (num % 8) * 5, "0x%02x ", buffer[num]);
    num++;
    if ((num % 8) == 0)
      DEBUG_LOG(SERIALINTERFACE, "%s", line);
  }
  if (num % 8)
    DEBUG_LOG(SERIALINTERFACE, "%s", line);
#endif
  return 0;
}

#ifdef HAS_LIBMGBA
#define SI_CASE_GBAEMU(call) SI_CASE(GBAEmu, CSIDevice_GBAEmu, call)
#else
#define SI_CASE_GBAEMU(call)
#endif

/* The call on the device class this is: its own, or its parent controller's, or, when it
 * has none, the default the base gives (named Fn##Default) */
#define SI_CASE(kind, D, call)                                                                     \
  case SIDeviceClass::kind:                                                                        \
    return call(static_cast<D*>(this));

#define SI_DISPATCH(call)                                                                          \
  switch (device_class)                                                                            \
  {                                                                                                \
    SI_CASE(GCController, CSIDevice_GCController, call)                                            \
    SI_CASE(TaruKonga, CSIDevice_TaruKonga, call)                                                  \
    SI_CASE(GCAdapter, CSIDevice_GCAdapter, call)                                                  \
    SI_CASE(DanceMat, CSIDevice_DanceMat, call)                                                    \
    SI_CASE(GCSteeringWheel, CSIDevice_GCSteeringWheel, call)                                      \
    SI_CASE(GBA, CSIDevice_GBA, call)                                                              \
    SI_CASE_GBAEMU(call)                                                                           \
    SI_CASE(Keyboard, CSIDevice_Keyboard, call)                                                    \
    SI_CASE(AMBaseboard, CSIDevice_AMBaseboard, call)                                              \
  case SIDeviceClass::Null:                                                                        \
  default:                                                                                         \
    break;                                                                                         \
  }                                                                                                \
  return call(static_cast<CSIDevice_Null*>(this));

// Whether D has a call of its own, or a parent's other than the base's
#define SI_HAS_OWN(D, Fn) (!std::is_same_v<decltype(&D::Fn), decltype(&ISIDevice::Fn)>)

int ISIDevice::RunBuffer(u8* buffer, int request_length)
{
  const auto call = [&](auto* device) -> int {
    using D = std::remove_pointer_t<decltype(device)>;
    if constexpr (SI_HAS_OWN(D, RunBuffer))
      return device->RunBuffer(buffer, request_length);
    else
      return RunBufferDefault(buffer, request_length);
  };
  SI_DISPATCH(call)
}

int ISIDevice::TransferInterval()
{
  const auto call = [&](auto* device) -> int {
    using D = std::remove_pointer_t<decltype(device)>;
    if constexpr (SI_HAS_OWN(D, TransferInterval))
      return device->TransferInterval();
    else
      return TransferIntervalDefault();
  };
  SI_DISPATCH(call)
}

DataResponse ISIDevice::GetData(u32& hi, u32& low)
{
  const auto call = [&](auto* device) -> DataResponse {
    using D = std::remove_pointer_t<decltype(device)>;
    static_assert(SI_HAS_OWN(D, GetData), "every device has its own GetData");
    return device->GetData(hi, low);
  };
  SI_DISPATCH(call)
}

void ISIDevice::SendCommand(u32 command, u8 poll)
{
  const auto call = [&](auto* device) {
    using D = std::remove_pointer_t<decltype(device)>;
    static_assert(SI_HAS_OWN(D, SendCommand), "every device has its own SendCommand");
    device->SendCommand(command, poll);
  };
  SI_DISPATCH(call)
}

void ISIDevice::DoState(PointerWrap& p)
{
  const auto call = [&](auto* device) {
    using D = std::remove_pointer_t<decltype(device)>;
    if constexpr (SI_HAS_OWN(D, DoState))
      device->DoState(p);
    else
      DoStateDefault(p);
  };
  SI_DISPATCH(call)
}

void ISIDevice::OnEvent(u64 userdata, s64 cycles_late)
{
  const auto call = [&](auto* device) {
    using D = std::remove_pointer_t<decltype(device)>;
    if constexpr (SI_HAS_OWN(D, OnEvent))
      device->OnEvent(userdata, cycles_late);
    else
      OnEventDefault(userdata, cycles_late);
  };
  SI_DISPATCH(call)
}

int ISIDevice::CreateStatusResponse(u32 si_device_id, u8* buffer)
{
  constexpr int RESPONSE_LENGTH = 3;

  Common::BigEndianValue<u32> id(si_device_id);
  std::memcpy(buffer, &id, RESPONSE_LENGTH);
  return RESPONSE_LENGTH;
}

int SIDevice_GetGBATransferTime(const SystemTimers::SystemTimersManager& timers,
                                EBufferCommands cmd)
{
  u64 gc_bytes_transferred = 1;
  u64 gba_bytes_transferred = 1;
  const u64 stop_bits_ns = GC_STOP_BIT_NS + GBA_STOP_BIT_NS;

  switch (cmd)
  {
  case EBufferCommands::CMD_RESET:
  case EBufferCommands::CMD_STATUS:
  {
    gba_bytes_transferred = 3;
    break;
  }
  case EBufferCommands::CMD_READ_GBA:
  {
    gba_bytes_transferred = 5;
    break;
  }
  case EBufferCommands::CMD_WRITE_GBA:
  {
    gc_bytes_transferred = 5;
    break;
  }
  default:
  {
    gba_bytes_transferred = 0;
    break;
  }
  }

  const u32 ticks_per_second = timers.GetTicksPerSecond();
  const u64 cycles = (gba_bytes_transferred * 8 * ticks_per_second / GBA_BITS_PER_SECOND) +
                     (gc_bytes_transferred * 8 * ticks_per_second / GC_BITS_PER_SECOND) +
                     (stop_bits_ns * ticks_per_second / 1000000000LL);
  return static_cast<int>(cycles);
}

// Check if a device class is inheriting from CSIDevice_GCController
// The goal of this function is to avoid special casing a long list of
// device types when there is no "real" input device, e.g. when playing
// a TAS movie, or netplay input.
bool SIDevice_IsGCController(SIDevices type)
{
  switch (type)
  {
  case SIDEVICE_GC_CONTROLLER:
  case SIDEVICE_WIIU_ADAPTER:
  case SIDEVICE_GC_TARUKONGA:
  case SIDEVICE_DANCEMAT:
  case SIDEVICE_GC_STEERING:
  case SIDEVICE_AM_BASEBOARD:
    return true;
  default:
    return false;
  }
}

// F A C T O R Y
ISIDevice* SIDevice_Create(Core::System& system, const SIDevices device, const int port_number)
{
  switch (device)
  {
  case SIDEVICE_GC_CONTROLLER:
    return new CSIDevice_GCController(system, device, port_number);

  case SIDEVICE_WIIU_ADAPTER:
    return new CSIDevice_GCAdapter(system, device, port_number);

  case SIDEVICE_DANCEMAT:
    return new CSIDevice_DanceMat(system, device, port_number);

  case SIDEVICE_GC_STEERING:
    return new CSIDevice_GCSteeringWheel(system, device, port_number);

  case SIDEVICE_GC_TARUKONGA:
    return new CSIDevice_TaruKonga(system, device, port_number);

  case SIDEVICE_GC_GBA:
    return new CSIDevice_GBA(system, device, port_number);

  case SIDEVICE_GC_GBA_EMULATED:
#ifdef HAS_LIBMGBA
    return new CSIDevice_GBAEmu(system, device, port_number);
#else
    PanicAlert("Error: This build does not support emulated GBA controllers");
    return new CSIDevice_Null(system, device, port_number);
#endif

  case SIDEVICE_GC_KEYBOARD:
    return new CSIDevice_Keyboard(system, device, port_number);

  case SIDEVICE_AM_BASEBOARD:
    return new CSIDevice_AMBaseboard(system, device, port_number);

  case SIDEVICE_NONE:
  default:
    return new CSIDevice_Null(system, device, port_number);
  }
}

void SIDevice_Destroy(ISIDevice* device)
{
  if (!device)
    return;
  const auto call = [](auto* d) { delete d; };
  switch (device->device_class)
  {
  case SIDeviceClass::GCController:
    return call(static_cast<CSIDevice_GCController*>(device));
  case SIDeviceClass::TaruKonga:
    return call(static_cast<CSIDevice_TaruKonga*>(device));
  case SIDeviceClass::GCAdapter:
    return call(static_cast<CSIDevice_GCAdapter*>(device));
  case SIDeviceClass::DanceMat:
    return call(static_cast<CSIDevice_DanceMat*>(device));
  case SIDeviceClass::GCSteeringWheel:
    return call(static_cast<CSIDevice_GCSteeringWheel*>(device));
  case SIDeviceClass::GBA:
    return call(static_cast<CSIDevice_GBA*>(device));
#ifdef HAS_LIBMGBA
  case SIDeviceClass::GBAEmu:
    return call(static_cast<CSIDevice_GBAEmu*>(device));
#endif
  case SIDeviceClass::Keyboard:
    return call(static_cast<CSIDevice_Keyboard*>(device));
  case SIDeviceClass::AMBaseboard:
    return call(static_cast<CSIDevice_AMBaseboard*>(device));
  case SIDeviceClass::Null:
  default:
    return call(static_cast<CSIDevice_Null*>(device));
  }
}
}  // namespace SerialInterface
