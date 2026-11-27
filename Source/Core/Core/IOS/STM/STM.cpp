// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/STM/STM.h"

#include <functional>
#include <memory>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "DolphinLibretro/Common/Globals.h"

namespace IOS::HLE
{
// The pending event hook request's address, 0 for none: the request stays in guest memory
// until it is answered
static u32 s_event_hook_address;

IPCReply STMImmediateDevice::IOCtl(const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  s32 return_value = IPC_SUCCESS;
  switch (request.request)
  {
  case IOCTL_STM_IDLE:
  case IOCTL_STM_SHUTDOWN:
    NOTICE_LOG(IOS_STM, "IOCTL_STM_IDLE or IOCTL_STM_SHUTDOWN received, shutting down");
    Libretro::RequestShutdown();
    break;

  case IOCTL_STM_RELEASE_EH:
  {
    if (!s_event_hook_address)
    {
      return_value = IPC_ENOENT;
      break;
    }
    const IOCtlRequest hook(system, s_event_hook_address);
    memory.Write_U32(0, hook.buffer_out);
    GetEmulationKernel().EnqueueIPCReply(hook, IPC_SUCCESS);
    s_event_hook_address = 0;
    break;
  }

  case IOCTL_STM_HOTRESET:
    INFO_LOG(IOS_STM, "%s - IOCtl:", GetDeviceName());
    INFO_LOG(IOS_STM, "    IOCTL_STM_HOTRESET");
    break;

  case IOCTL_STM_VIDIMMING:  // (Input: 20 bytes, Output: 20 bytes)
    INFO_LOG(IOS_STM, "%s - IOCtl:", GetDeviceName());
    INFO_LOG(IOS_STM, "    IOCTL_STM_VIDIMMING");
    // memory.Write_U32(1, buffer_out);
    // return_value = 1;
    break;

  case IOCTL_STM_LEDMODE:  // (Input: 20 bytes, Output: 20 bytes)
    INFO_LOG(IOS_STM, "%s - IOCtl:", GetDeviceName());
    INFO_LOG(IOS_STM, "    IOCTL_STM_LEDMODE");
    break;

  case IOCTL_STM_HOTRESET_FOR_PD:
  case IOCTL_STM_WAKEUP:
  case IOCTL_STM_GET_IDLEMODE:
  case IOCTL_STM_READDDRREG:
  case IOCTL_STM_READDDRREG2:
  case IOCTL_STM_LEDFLASH:
  case IOCTL_STM_READVER:
  case IOCTL_STM_WRITEDMCU:
    ERROR_LOG(IOS_STM, "%s - Unimplemented IOCtl: %u", GetDeviceName(), request.request);
    break;

  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_STM);
    return_value = IPC_UNKNOWN;
    break;
  }

  return IPCReply(return_value);
}

STMEventHookDevice::~STMEventHookDevice()
{
  s_event_hook_address = 0;
}

IPCReply STMEventHookDevice::IOCtl(const IOCtlRequest& request)
{
  if (request.request != IOCTL_STM_EVENTHOOK)
    return IPCReply(IPC_UNKNOWN);

  if (s_event_hook_address)
    return IPCReply(IPC_EEXIST);

  // IOCTL_STM_EVENTHOOK waits until the reset button or power button is pressed.
  s_event_hook_address = request.address;
  return IPCReply::None();
}

void STMEventHookDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  p.Do(s_event_hook_address);
}

bool STMEventHookDevice::HasHookInstalled() const
{
  return s_event_hook_address != 0;
}

void STMEventHookDevice::TriggerEvent(const u32 event) const
{
  // If the device isn't open, ignore the button press.
  if (!m_is_active || !s_event_hook_address)
    return;

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const IOCtlRequest hook(system, s_event_hook_address);
  memory.Write_U32(event, hook.buffer_out);
  GetEmulationKernel().EnqueueIPCReply(hook, IPC_SUCCESS);
  s_event_hook_address = 0;
}

void STMEventHookDevice::ResetButton() const
{
  // The reset button triggers STM_EVENT_RESET.
  TriggerEvent(STM_EVENT_RESET);
}

void STMEventHookDevice::PowerButton() const
{
  TriggerEvent(STM_EVENT_POWER);
}
}  // namespace IOS::HLE
