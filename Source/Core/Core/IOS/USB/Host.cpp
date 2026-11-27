// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Host.h"

#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBScanner.h"
#include "Core/System.h"

#ifdef _WIN32
#include "Common/WindowsDevice.h"
#endif

namespace IOS::HLE
{
static void FreePendingDevices(USBDeviceList* list)
{
  if (!list)
    return;
  usb_device_list_clear(list);
  delete list;
}

USBHost::USBHost(EmulationKernel& ios, const char* device_name)
    : EmulationDevice(ios, device_name)
{
}

USBHost::~USBHost()
{
  GetSystem().GetUSBScanner().RemoveClient(this);
  FreePendingDevices(
      static_cast<USBDeviceList*>(retro_atomic_exchange_ptr(&m_pending_devices, nullptr)));
  usb_device_list_clear(&m_devices);
}

IPCReply USBHost::Open(const OpenRequest& request)
{
  if (!m_has_initialised)
  {
    GetSystem().GetUSBScanner().AddClient(this);
    // Force a device scan to complete, because some games (including Your Shape) only care
    // about the initial device list (in the first GETDEVICECHANGE reply).
    GetSystem().GetUSBScanner().WaitForFirstScan();
    UpdateFromScanner();
    m_has_initialised = true;
  }
  return IPCReply(IPC_SUCCESS);
}

void USBHost::DoState(PointerWrap& p)
{
  Device::DoState(p);
  if (IsOpened() && p.IsReadMode())
  {
    // After a state has loaded, there may be insertion hooks for devices that were
    // already plugged in, and which need to be triggered.
    usb_device_list_clear(&m_devices);
    UpdateFromScanner();
  }
}

USB::Device* USBHost::GetDeviceById(const u64 device_id) const
{
  return usb_device_list_find(&m_devices, device_id);
}

void USBHost::OnDeviceChange(ChangeEvent event, USB::Device* changed_device)
{
}

void USBHost::OnDeviceChangeEnd()
{
}

bool USBHost::ShouldAddDevice(const USB::Device& device) const
{
  return true;
}

void USBHost::Update()
{
  if (USBDeviceList* const pending =
          static_cast<USBDeviceList*>(retro_atomic_exchange_ptr(&m_pending_devices, nullptr)))
  {
    OnDevicesChangedInternal(pending);
    FreePendingDevices(pending);
  }
}

void USBHost::UpdateFromScanner()
{
  USBDeviceList devices;
  GetSystem().GetUSBScanner().GetDevices(&devices);
  OnDevicesChangedInternal(&devices);
  usb_device_list_clear(&devices);
}

// Scan thread: only the newest list matters, so it replaces any still waiting.
void USBHost::OnDevicesChanged(const USBDeviceList* new_devices)
{
  USBDeviceList* const copy = new USBDeviceList;
  usb_device_list_copy(copy, new_devices);
  FreePendingDevices(
      static_cast<USBDeviceList*>(retro_atomic_exchange_ptr(&m_pending_devices, copy)));
}

void USBHost::OnDevicesChangedInternal(const USBDeviceList* new_devices)
{
  bool changes = false;

  u32 i = 0;
  while (i < m_devices.count)
  {
    if (usb_device_list_find(new_devices, m_devices.ids[i]))
    {
      i++;
      continue;
    }
    USB::Device* const device = usb_device_list_take(&m_devices, i);
    INFO_LOG(IOS_USB, "%s - Removed device: %04x:%04x", GetDeviceName(),
             (unsigned)device->GetVid(), (unsigned)device->GetPid());

    changes = true;
    OnDeviceChange(ChangeEvent::Removed, device);
    device->Release();
  }

  for (i = 0; i < new_devices->count; i++)
  {
    USB::Device* const device = new_devices->devices[i];
    if (!usb_device_list_find(&m_devices, new_devices->ids[i]) && ShouldAddDevice(*device))
    {
      INFO_LOG(IOS_USB, "%s - New device: %04x:%04x", GetDeviceName(),
               (unsigned)device->GetVid(), (unsigned)device->GetPid());

      changes = true;
      device->AddRef();
      usb_device_list_add(&m_devices, device);
      OnDeviceChange(ChangeEvent::Inserted, device);
    }
  }

  if (changes)
    OnDeviceChangeEnd();
}

IPCReply USBHost::TransferReply(const USB::Device& device, u32 request, s32 ret) const
{
  if (ret == IPC_SUCCESS)
    return IPCReply::None();

  ERROR_LOG(IOS_USB, "[%04x:%04x] Failed to submit transfer (request %u): %s",
            (unsigned)device.GetVid(), (unsigned)device.GetPid(), request,
            device.GetErrorName(ret).c_str());
  return IPCReply(ret <= 0 ? ret : IPC_EINVAL);
}

}  // namespace IOS::HLE
