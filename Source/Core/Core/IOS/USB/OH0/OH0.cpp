// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/OH0/OH0.h"

#include <algorithm>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBV0.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/System.h"

namespace IOS::HLE
{
OH0::OH0(EmulationKernel& ios, const std::string& device_name) : USBHost(ios, device_name)
{
}

OH0::~OH0() = default;

IPCReply OH0::Open(const OpenRequest& request)
{
  if (HasFeature(m_ios.GetVersion(), Feature::NewUSB))
    return IPCReply(IPC_EACCES);
  return USBHost::Open(request);
}

IPCReply OH0::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB);
  switch (request.request)
  {
  case USB::IOCTL_USBV0_GETRHDESCA:
    return GetRhDesca(request);
  case USB::IOCTL_USBV0_CANCEL_INSERT_HOOK:
    return CancelInsertionHook(request);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

IPCReply OH0::IOCtlV(const IOCtlVRequest& request)
{
  INFO_LOG(IOS_USB, "/dev/usb/oh0 - IOCtlV %u", request.request);
  switch (request.request)
  {
  case USB::IOCTLV_USBV0_GETDEVLIST:
    return GetDeviceList(request);
  case USB::IOCTLV_USBV0_GETRHPORTSTATUS:
    return GetRhPortStatus(request);
  case USB::IOCTLV_USBV0_SETRHPORTSTATUS:
    return SetRhPortStatus(request);
  case USB::IOCTLV_USBV0_DEVINSERTHOOK:
    return RegisterInsertionHook(request);
  case USB::IOCTLV_USBV0_DEVICECLASSCHANGE:
    return RegisterClassChangeHook(request);
  case USB::IOCTLV_USBV0_DEVINSERTHOOKID:
    return RegisterInsertionHookWithID(request);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

void OH0::DoState(PointerWrap& p)
{
  {
    if (p.IsReadMode() && m_devices.count != 0)
    {
      Core::DisplayMessage("It is suggested that you unplug and replug all connected USB devices.");
      Core::DisplayMessage("If USB doesn't work properly, an emulation reset may be needed.");
    }
    // as a map, a map and a set: each a count and then its entries in order
    u32 count = m_insertion_hook_count;
    p.Do(count);
    if (count > MAX_ENTRIES)
    {
      p.SetMeasureMode();
      return;
    }
    m_insertion_hook_count = count;
    for (u32 i = 0; i < count; i++)
    {
      p.Do(m_insertion_hooks[i].info);
      p.Do(m_insertion_hooks[i].address);
    }

    count = m_removal_hook_count;
    p.Do(count);
    if (count > MAX_ENTRIES)
    {
      p.SetMeasureMode();
      return;
    }
    m_removal_hook_count = count;
    for (u32 i = 0; i < count; i++)
    {
      p.Do(m_removal_hooks[i].device_id);
      p.Do(m_removal_hooks[i].address);
    }

    count = m_opened_device_count;
    p.Do(count);
    if (count > MAX_ENTRIES)
    {
      p.SetMeasureMode();
      return;
    }
    m_opened_device_count = count;
    for (u32 i = 0; i < count; i++)
      p.Do(m_opened_devices[i]);
  }
  USBHost::DoState(p);
}

IPCReply OH0::CancelInsertionHook(const IOCtlRequest& request)
{
  if (!request.buffer_in || request.buffer_in_size != 4)
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  // IOS assigns random IDs, but ours are simply the VID + PID (see RegisterInsertionHookWithID)
  TriggerInsertionHook({memory.Read_U16(request.buffer_in), memory.Read_U16(request.buffer_in + 2)},
                       USB_ECANCELED);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::GetDeviceList(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(2, 2))
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8 max_entries_count = memory.Read_U8(request.in_vectors[0].address);
  if (request.io_vectors[1].size != max_entries_count * sizeof(DeviceEntry))
    return IPCReply(IPC_EINVAL);

  const u8 interface_class = memory.Read_U8(request.in_vectors[1].address);
  u8 entries_count = 0;
  for (u32 i = 0; i < m_devices.count; i++)
  {
    const USB::Device* const device = m_devices.devices[i];
    if (entries_count >= max_entries_count)
      break;
    if (!device->HasClass(interface_class))
      continue;

    DeviceEntry entry;
    entry.unknown = 0;
    entry.vid = Common::swap16(device->GetVid());
    entry.pid = Common::swap16(device->GetPid());
    memory.CopyToEmu(request.io_vectors[1].address + 8 * entries_count++, &entry, 8);
  }
  memory.Write_U8(entries_count, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::GetRhDesca(const IOCtlRequest& request) const
{
  if (!request.buffer_out || request.buffer_out_size != 4)
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  // Based on a hardware test, this ioctl seems to return a constant value
  memory.Write_U32(0x02000302, request.buffer_out);
  request.Dump(system, GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LWARNING);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::GetRhPortStatus(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(IPC_EINVAL);

  ERROR_LOG(IOS_USB, "Unimplemented IOCtlV: IOCTLV_USBV0_GETRHPORTSTATUS");
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LERROR);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::SetRhPortStatus(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(IPC_EINVAL);

  ERROR_LOG(IOS_USB, "Unimplemented IOCtlV: IOCTLV_USBV0_SETRHPORTSTATUS");
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LERROR);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::RegisterRemovalHook(const u64 device_id, const IOCtlRequest& request)
{
  // IOS only allows a single device removal hook.
  u32 slot = 0;
  while (slot < m_removal_hook_count && m_removal_hooks[slot].device_id < device_id)
    slot++;
  if (slot < m_removal_hook_count && m_removal_hooks[slot].device_id == device_id)
    return IPCReply(IPC_EEXIST);
  if (m_removal_hook_count == MAX_ENTRIES)
  {
    ERROR_LOG(IOS_USB, "Too many device removal hooks");
    return IPCReply(IPC_EEXIST);
  }
  for (u32 i = m_removal_hook_count; i > slot; i--)
    m_removal_hooks[i] = m_removal_hooks[i - 1];
  m_removal_hooks[slot].device_id = device_id;
  m_removal_hooks[slot].address = request.address;
  m_removal_hook_count++;
  return IPCReply::None();
}

IPCReply OH0::RegisterInsertionHook(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u16 vid = memory.Read_U16(request.in_vectors[0].address);
  const u16 pid = memory.Read_U16(request.in_vectors[1].address);
  if (HasDeviceWithVidPid(vid, pid))
    return IPCReply(IPC_SUCCESS);
  // TODO: figure out whether IOS allows more than one hook.
  SetInsertionHook({vid, pid}, request.address, true);
  return IPCReply::None();
}

IPCReply OH0::RegisterInsertionHookWithID(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 1))
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u16 vid = memory.Read_U16(request.in_vectors[0].address);
  const u16 pid = memory.Read_U16(request.in_vectors[1].address);
  const bool trigger_only_for_new_device = memory.Read_U8(request.in_vectors[2].address) == 1;
  if (!trigger_only_for_new_device && HasDeviceWithVidPid(vid, pid))
    return IPCReply(IPC_SUCCESS);
  // TODO: figure out whether IOS allows more than one hook.
  SetInsertionHook({vid, pid}, request.address, false);
  // The output vector is overwritten with an ID to use with ioctl 31 for cancelling the hook.
  memory.Write_U32(vid << 16 | pid, request.io_vectors[0].address);
  return IPCReply::None();
}

IPCReply OH0::RegisterClassChangeHook(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return IPCReply(IPC_EINVAL);
  WARN_LOG(IOS_USB, "Unimplemented IOCtlV: USB::IOCTLV_USBV0_DEVICECLASSCHANGE (no reply)");
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LWARNING);
  return IPCReply::None();
}

bool OH0::HasDeviceWithVidPid(const u16 vid, const u16 pid) const
{
  for (u32 i = 0; i < m_devices.count; i++)
  {
    if (m_devices.devices[i]->GetVid() == vid && m_devices.devices[i]->GetPid() == pid)
      return true;
  }
  return false;
}

void OH0::OnDeviceChange(const ChangeEvent event, USB::Device* device)
{
  if (event == ChangeEvent::Inserted)
  {
    TriggerInsertionHook({device->GetVid(), device->GetPid()}, IPC_SUCCESS);
  }
  else if (event == ChangeEvent::Removed)
  {
    TriggerRemovalHook(device->GetId(), IPC_SUCCESS);

    // This fixes a problem where Rock Band 3 randomly fails to detect reconnected microphones.
    // Real IOS behavior untested.
    SetDeviceOpened(device->GetId(), false);
  }
}

void OH0::ReplyToHook(u32 address, ReturnCode return_value)
{
  GetEmulationKernel().EnqueueIPCReply(Request{GetSystem(), address}, return_value, 0,
                                       CoreTiming::FromThread::ANY);
}

// Hooks are keyed by VID alone, as DeviceInfo orders them: a hook for a VID already hooked keeps
// that hook's PID.
void OH0::SetInsertionHook(USB::DeviceInfo info, u32 address, bool replace)
{
  u32 slot = 0;
  while (slot < m_insertion_hook_count && m_insertion_hooks[slot].info.vid < info.vid)
    slot++;
  if (slot < m_insertion_hook_count && m_insertion_hooks[slot].info.vid == info.vid)
  {
    if (replace)
      m_insertion_hooks[slot].address = address;
    return;
  }
  if (m_insertion_hook_count == MAX_ENTRIES)
  {
    ERROR_LOG(IOS_USB, "Too many device insertion hooks");
    return;
  }
  for (u32 i = m_insertion_hook_count; i > slot; i--)
    m_insertion_hooks[i] = m_insertion_hooks[i - 1];
  m_insertion_hooks[slot].info = info;
  m_insertion_hooks[slot].address = address;
  m_insertion_hook_count++;
}

void OH0::TriggerInsertionHook(USB::DeviceInfo info, ReturnCode return_value)
{
  for (u32 slot = 0; slot < m_insertion_hook_count; slot++)
  {
    if (m_insertion_hooks[slot].info.vid == info.vid)
    {
      const u32 address = m_insertion_hooks[slot].address;
      for (u32 i = slot; i + 1 < m_insertion_hook_count; i++)
        m_insertion_hooks[i] = m_insertion_hooks[i + 1];
      m_insertion_hook_count--;
      ReplyToHook(address, return_value);
      return;
    }
  }
}

void OH0::TriggerRemovalHook(u64 device_id, ReturnCode return_value)
{
  for (u32 slot = 0; slot < m_removal_hook_count; slot++)
  {
    if (m_removal_hooks[slot].device_id == device_id)
    {
      const u32 address = m_removal_hooks[slot].address;
      for (u32 i = slot; i + 1 < m_removal_hook_count; i++)
        m_removal_hooks[i] = m_removal_hooks[i + 1];
      m_removal_hook_count--;
      ReplyToHook(address, return_value);
      return;
    }
  }
}

bool OH0::IsDeviceOpened(u64 device_id) const
{
  for (u32 i = 0; i < m_opened_device_count; i++)
  {
    if (m_opened_devices[i] == device_id)
      return true;
  }
  return false;
}

void OH0::SetDeviceOpened(u64 device_id, bool opened)
{
  u32 slot = 0;
  while (slot < m_opened_device_count && m_opened_devices[slot] < device_id)
    slot++;
  const bool present = slot < m_opened_device_count && m_opened_devices[slot] == device_id;
  if (opened && !present)
  {
    if (m_opened_device_count == MAX_ENTRIES)
    {
      ERROR_LOG(IOS_USB, "Too many opened devices");
      return;
    }
    for (u32 i = m_opened_device_count; i > slot; i--)
      m_opened_devices[i] = m_opened_devices[i - 1];
    m_opened_devices[slot] = device_id;
    m_opened_device_count++;
  }
  else if (!opened && present)
  {
    for (u32 i = slot; i + 1 < m_opened_device_count; i++)
      m_opened_devices[i] = m_opened_devices[i + 1];
    m_opened_device_count--;
  }
}

ReturnCode OH0::DeviceOpen(const u16 vid, const u16 pid, u64* device_id)
{

  bool has_device_with_vid_pid = false;
  for (u32 i = 0; i < m_devices.count; i++)
  {
    USB::Device* const device = m_devices.devices[i];
    if (device->GetVid() != vid || device->GetPid() != pid)
      continue;
    has_device_with_vid_pid = true;

    if (IsDeviceOpened(device->GetId()) || !device->Attach())
    {
      continue;
    }

    SetDeviceOpened(device->GetId(), true);
    *device_id = device->GetId();
    return IPC_SUCCESS;
  }
  // IOS doesn't allow opening the same device more than once (IPC_EEXIST)
  *device_id = 0;
  return has_device_with_vid_pid ? IPC_EEXIST : IPC_ENOENT;
}

void OH0::DeviceClose(const u64 device_id)
{
  TriggerRemovalHook(device_id, IPC_ENOENT);
  SetDeviceOpened(device_id, false);
}

IPCReply OH0::DeviceIOCtl(const u64 device_id, const IOCtlRequest& request)
{
  const auto device = GetDeviceById(device_id);
  if (!device)
    return IPCReply(IPC_ENOENT);

  switch (request.request)
  {
  case USB::IOCTL_USBV0_DEVREMOVALHOOK:
    return RegisterRemovalHook(device_id, request);
  case USB::IOCTL_USBV0_SUSPENDDEV:
  case USB::IOCTL_USBV0_RESUMEDEV:
    // Unimplemented because libusb doesn't do power management.
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV0_RESET_DEVICE:
    TriggerRemovalHook(device_id, IPC_SUCCESS);
    return IPCReply(IPC_SUCCESS);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

IPCReply OH0::DeviceIOCtlV(const u64 device_id, const IOCtlVRequest& request)
{
  const auto device = GetDeviceById(device_id);
  if (!device)
    return IPCReply(IPC_ENOENT);

  switch (request.request)
  {
  case USB::IOCTLV_USBV0_CTRLMSG:
  case USB::IOCTLV_USBV0_BLKMSG:
  case USB::IOCTLV_USBV0_LBLKMSG:
  case USB::IOCTLV_USBV0_INTRMSG:
  case USB::IOCTLV_USBV0_ISOMSG:
    if (!device)
      return IPCReply(IPC_ENOENT);
    return TransferReply(*device, request.request, SubmitTransfer(*device, request));
  case USB::IOCTLV_USBV0_UNKNOWN_32:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
    return IPCReply(IPC_SUCCESS);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

s32 OH0::SubmitTransfer(USB::Device& device, const IOCtlVRequest& ioctlv)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV0_CTRLMSG:
    if (!ioctlv.HasNumberOfValidVectors(6, 1) ||
        Common::swap16(memory.Read_U16(ioctlv.in_vectors[4].address)) != ioctlv.io_vectors[0].size)
      return IPC_EINVAL;
    return device.SubmitTransfer(new USB::V0CtrlMessage(GetEmulationKernel(), ioctlv));

  case USB::IOCTLV_USBV0_BLKMSG:
  case USB::IOCTLV_USBV0_LBLKMSG:
    if (!ioctlv.HasNumberOfValidVectors(2, 1) ||
        memory.Read_U16(ioctlv.in_vectors[1].address) != ioctlv.io_vectors[0].size)
      return IPC_EINVAL;
    return device.SubmitTransfer(new USB::V0BulkMessage(
        GetEmulationKernel(), ioctlv, ioctlv.request == USB::IOCTLV_USBV0_LBLKMSG));

  case USB::IOCTLV_USBV0_INTRMSG:
    if (!ioctlv.HasNumberOfValidVectors(2, 1) ||
        memory.Read_U16(ioctlv.in_vectors[1].address) != ioctlv.io_vectors[0].size)
      return IPC_EINVAL;
    return device.SubmitTransfer(new USB::V0IntrMessage(GetEmulationKernel(), ioctlv));

  case USB::IOCTLV_USBV0_ISOMSG:
    if (!ioctlv.HasNumberOfValidVectors(3, 2))
      return IPC_EINVAL;
    return device.SubmitTransfer(new USB::V0IsoMessage(GetEmulationKernel(), ioctlv));

  default:
    return IPC_EINVAL;
  }
}
}  // namespace IOS::HLE
