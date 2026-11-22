// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/USB_HID/HIDv4.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBV4.h"
#include "Core/System.h"

namespace IOS::HLE
{
USB_HIDv4::USB_HIDv4(EmulationKernel& ios, const std::string& device_name)
    : USBHost(ios, device_name)
{
}

USB_HIDv4::~USB_HIDv4() = default;

IPCReply USB_HIDv4::IOCtl(const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB, Common::Log::LogLevel::LDEBUG);
  switch (request.request)
  {
  case USB::IOCTL_USBV4_GETVERSION:
    return IPCReply(VERSION);
  case USB::IOCTL_USBV4_GETDEVICECHANGE:
    return GetDeviceChange(request);
  case USB::IOCTL_USBV4_SHUTDOWN:
    return Shutdown(request);
  case USB::IOCTL_USBV4_SET_SUSPEND:
    // Not implemented in IOS
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV4_CANCELINTERRUPT:
    return CancelInterrupt(request);
  case USB::IOCTL_USBV4_GET_US_STRING:
  case USB::IOCTL_USBV4_CTRLMSG:
  case USB::IOCTL_USBV4_INTRMSG_IN:
  case USB::IOCTL_USBV4_INTRMSG_OUT:
  {
    if (request.buffer_in == 0 || request.buffer_in_size != 32)
      return IPCReply(IPC_EINVAL);
    const auto device = GetDeviceByIOSID(memory.Read_U32(request.buffer_in + 16));
    if (!device)
      return IPCReply(IPC_ENOENT);
    if (!device->Attach())
      return IPCReply(IPC_EINVAL);
    return TransferReply(*device, request.request, SubmitTransfer(*device, request));
  }
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
    return IPCReply(IPC_SUCCESS);
  }
}

IPCReply USB_HIDv4::CancelInterrupt(const IOCtlRequest& request)
{
  if (request.buffer_in == 0 || request.buffer_in_size != 8)
    return IPCReply(IPC_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  auto device = GetDeviceByIOSID(memory.Read_U32(request.buffer_in));
  if (!device)
    return IPCReply(IPC_ENOENT);
  device->CancelTransfer(memory.Read_U8(request.buffer_in + 4));
  return IPCReply(IPC_SUCCESS);
}

IPCReply USB_HIDv4::GetDeviceChange(const IOCtlRequest& request)
{
  if (request.buffer_out == 0 || request.buffer_out_size != 0x600)
    return IPCReply(IPC_EINVAL);

  m_devicechange_hook_address = request.address;
  // If there are pending changes, the reply is sent immediately (instead of on device
  // insertion/removal).
  if (m_has_pending_changes || m_is_shut_down)
  {
    TriggerDeviceChangeReply();
    m_has_pending_changes = false;
    m_is_shut_down = false;
  }
  return IPCReply::None();
}

IPCReply USB_HIDv4::Shutdown(const IOCtlRequest& request)
{
  if (m_devicechange_hook_address)
  {
    auto& system = GetSystem();
    auto& memory = system.GetMemory();
    const IOCtlRequest hook(system, m_devicechange_hook_address);
    memory.Write_U32(0xffffffff, hook.buffer_out);
    GetEmulationKernel().EnqueueIPCReply(hook, -1);
    m_devicechange_hook_address = 0;
    m_is_shut_down = true;
  }
  return IPCReply(IPC_SUCCESS);
}

s32 USB_HIDv4::SubmitTransfer(USB::Device& device, const IOCtlRequest& request)
{
  switch (request.request)
  {
  case USB::IOCTL_USBV4_CTRLMSG:
    return device.SubmitTransfer(new USB::V4CtrlMessage(GetEmulationKernel(), request));
  case USB::IOCTL_USBV4_GET_US_STRING:
    return device.SubmitTransfer(new USB::V4GetUSStringMessage(GetEmulationKernel(), request));
  case USB::IOCTL_USBV4_INTRMSG_IN:
  case USB::IOCTL_USBV4_INTRMSG_OUT:
    return device.SubmitTransfer(new USB::V4IntrMessage(GetEmulationKernel(), request));
  default:
    return IPC_EINVAL;
  }
}

void USB_HIDv4::DoState(PointerWrap& p)
{
  p.Do(m_has_pending_changes);
  p.Do(m_devicechange_hook_address);

  // As two maps were: the IOS IDs and their devices in IOS ID order, then the devices and their
  // IOS IDs in device order
  u32 count = m_id_count;
  p.Do(count);
  if (p.IsReadMode())
  {
    if (count > MAX_DEVICES)
    {
      p.SetMeasureMode();
      return;
    }
    for (u32 i = 0; i < count; i++)
    {
      p.Do(m_ids[i].ios_id);
      p.Do(m_ids[i].device_id);
    }
    m_id_count = count;
    // the second map holds the same pairs
    u32 inverse_count = 0;
    p.Do(inverse_count);
    for (u32 i = 0; i < inverse_count; i++)
    {
      u64 device_id = 0;
      s32 ios_id = 0;
      p.Do(device_id);
      p.Do(ios_id);
    }
  }
  else
  {
    for (u32 i = 0; i < m_id_count; i++)
    {
      p.Do(m_ids[i].ios_id);
      p.Do(m_ids[i].device_id);
    }
    // the pairs by device: a sort of their indices, a handful at most
    u8 order[MAX_DEVICES];
    for (u32 i = 0; i < m_id_count; i++)
    {
      u32 j = i;
      for (; j > 0 && m_ids[order[j - 1]].device_id > m_ids[i].device_id; j--)
        order[j] = order[j - 1];
      order[j] = static_cast<u8>(i);
    }
    u32 inverse_count = m_id_count;
    p.Do(inverse_count);
    for (u32 i = 0; i < m_id_count; i++)
    {
      p.Do(m_ids[order[i]].device_id);
      p.Do(m_ids[order[i]].ios_id);
    }
  }

  USBHost::DoState(p);
}

std::shared_ptr<USB::Device> USB_HIDv4::GetDeviceByIOSID(const s32 ios_id) const
{
  for (u32 i = 0; i < m_id_count; i++)
  {
    if (m_ids[i].ios_id == ios_id)
      return GetDeviceById(m_ids[i].device_id);
  }
  return nullptr;
}

const USB_HIDv4::DeviceIds* USB_HIDv4::FindByDeviceId(u64 device_id) const
{
  for (u32 i = 0; i < m_id_count; i++)
  {
    if (m_ids[i].device_id == device_id)
      return &m_ids[i];
  }
  return nullptr;
}

void USB_HIDv4::OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device)
{
  {
    if (event == ChangeEvent::Inserted)
    {
      // the lowest free IOS ID, kept in order
      u32 slot = 0;
      while (slot < m_id_count && m_ids[slot].ios_id == static_cast<s32>(slot))
        slot++;
      if (m_id_count < MAX_DEVICES)
      {
        for (u32 i = m_id_count; i > slot; i--)
          m_ids[i] = m_ids[i - 1];
        m_ids[slot].ios_id = static_cast<s32>(slot);
        m_ids[slot].device_id = device->GetId();
        m_id_count++;
      }
      else
      {
        ERROR_LOG(IOS_USB, "Too many HID devices");
      }
    }
    else if (event == ChangeEvent::Removed)
    {
      if (const DeviceIds* const ids = FindByDeviceId(device->GetId()))
      {
        const u32 slot = static_cast<u32>(ids - m_ids);
        for (u32 i = slot; i + 1 < m_id_count; i++)
          m_ids[i] = m_ids[i + 1];
        m_id_count--;
      }
    }
  }

  {
    TriggerDeviceChangeReply();
  }
}

bool USB_HIDv4::ShouldAddDevice(const USB::Device& device) const
{
  return device.HasClass(HID_CLASS);
}

void USB_HIDv4::TriggerDeviceChangeReply()
{
  if (!m_devicechange_hook_address)
  {
    m_has_pending_changes = true;
    return;
  }

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const IOCtlRequest hook(system, m_devicechange_hook_address);

  {
    const u32 dest = hook.buffer_out;
    u32 offset = 0;
    for (const auto& device : m_devices)
    {
      const std::vector<u8> device_section = GetDeviceEntry(*device.second.get());
      if (offset + device_section.size() > hook.buffer_out_size - 1)
      {
        WARN_LOG(IOS_USB, "Too many devices connected, skipping");
        break;
      }
      memory.CopyToEmu(dest + offset, device_section.data(), device_section.size());
      offset += Common::AlignUp(static_cast<u32>(device_section.size()), 4);
    }
    // IOS writes 0xffffffff to the buffer when there are no more devices
    memory.Write_U32(0xffffffff, dest + offset);
  }

  GetEmulationKernel().EnqueueIPCReply(hook, IPC_SUCCESS, 0, CoreTiming::FromThread::ANY);
  m_devicechange_hook_address = 0;
}

template <typename T>
static void CopyDescriptorToBuffer(std::vector<u8>* buffer, T descriptor)
{
  const size_t size = sizeof(descriptor);
  descriptor.Swap();
  buffer->insert(buffer->end(), reinterpret_cast<const u8*>(&descriptor),
                 reinterpret_cast<const u8*>(&descriptor) + size);
  constexpr size_t number_of_padding_bytes = Common::AlignUp(size, 4) - size;
  buffer->insert(buffer->end(), number_of_padding_bytes, 0);
}

static std::vector<u8> GetDescriptors(const USB::Device& device)
{
  std::vector<u8> buffer;

  CopyDescriptorToBuffer(&buffer, device.GetDeviceDescriptor());
  const auto configurations = device.GetConfigurations();
  for (size_t c = 0; c < configurations.size(); ++c)
  {
    CopyDescriptorToBuffer(&buffer, configurations[c]);
    const auto interfaces = device.GetInterfaces(static_cast<u8>(c));
    for (size_t i = interfaces.size(); i-- > 0;)
    {
      CopyDescriptorToBuffer(&buffer, interfaces[i]);
      for (const auto& endpoint_descriptor : device.GetEndpoints(
               static_cast<u8>(c), interfaces[i].bInterfaceNumber, interfaces[i].bAlternateSetting))
      {
        CopyDescriptorToBuffer(&buffer, endpoint_descriptor);
      }
    }
  }
  return buffer;
}

std::vector<u8> USB_HIDv4::GetDeviceEntry(const USB::Device& device) const
{

  // The structure for a device section is as follows:
  //   0-4 bytes: total size of the device data, including the size and the device ID
  //   4-8 bytes: device ID
  //   the rest of the buffer is device descriptors data
  std::vector<u8> entry(8);
  const std::vector<u8> descriptors = GetDescriptors(device);
  const u32 entry_size = Common::swap32(static_cast<u32>(entry.size() + descriptors.size()));
  const DeviceIds* const ids = FindByDeviceId(device.GetId());
  const u32 ios_device_id = Common::swap32(static_cast<u32>(ids ? ids->ios_id : -1));
  std::memcpy(entry.data(), &entry_size, sizeof(entry_size));
  std::memcpy(entry.data() + 4, &ios_device_id, sizeof(ios_device_id));
  entry.insert(entry.end(), descriptors.begin(), descriptors.end());

  return entry;
}
}  // namespace IOS::HLE
