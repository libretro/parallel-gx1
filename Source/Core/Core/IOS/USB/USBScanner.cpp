// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/USBScanner.h"

#include <cstring>

#ifdef __LIBUSB__
#include <libusb.h>
#endif

#include "Common/FrameClock.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Infinity.h"
#include "Core/IOS/USB/Emulated/LogitechMic.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/WiiSpeak.h"
#include "Core/IOS/USB/Host.h"
#include "Core/IOS/USB/LibusbDevice.h"
#include "Core/System.h"
#include "Core/USBUtils.h"

namespace IOS::HLE
{
void usb_device_list_clear(USBDeviceList* list)
{
  for (u32 i = 0; i < list->count; i++)
    list->devices[i]->Release();
  list->count = 0;
}

void usb_device_list_copy(USBDeviceList* dst, const USBDeviceList* src)
{
  u32 i;
  for (i = 0; i < src->count; i++)
  {
    dst->ids[i] = src->ids[i];
    dst->devices[i] = src->devices[i];
    dst->devices[i]->AddRef();
  }
  dst->count = src->count;
}

static u32 usb_device_list_lower_bound(const USBDeviceList* list, u64 id)
{
  u32 lo = 0;
  u32 hi = list->count;
  while (lo < hi)
  {
    const u32 mid = lo + (hi - lo) / 2;
    if (list->ids[mid] < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

USB::Device* usb_device_list_find(const USBDeviceList* list, u64 id)
{
  const u32 i = usb_device_list_lower_bound(list, id);
  return i < list->count && list->ids[i] == id ? list->devices[i] : nullptr;
}

void usb_device_list_add(USBDeviceList* list, USB::Device* device)
{
  const u64 id = device->GetId();
  const u32 i = usb_device_list_lower_bound(list, id);
  if (i < list->count && list->ids[i] == id)
  {
    list->devices[i]->Release();
    list->devices[i] = device;
    return;
  }
  if (list->count == USBDeviceList::MAX_DEVICES)
  {
    ERROR_LOG(IOS_USB, "Too many USB devices");
    device->Release();
    return;
  }
  memmove(&list->ids[i + 1], &list->ids[i], (list->count - i) * sizeof(list->ids[0]));
  memmove(&list->devices[i + 1], &list->devices[i], (list->count - i) * sizeof(list->devices[0]));
  list->ids[i] = id;
  list->devices[i] = device;
  list->count++;
}

USB::Device* usb_device_list_take(USBDeviceList* list, u32 index)
{
  USB::Device* const device = list->devices[index];
  list->count--;
  memmove(&list->ids[index], &list->ids[index + 1], (list->count - index) * sizeof(list->ids[0]));
  memmove(&list->devices[index], &list->devices[index + 1],
          (list->count - index) * sizeof(list->devices[0]));
  return device;
}

USBScanner::~USBScanner()
{
  StopScanning();
  if (Devices* const devices =
          static_cast<Devices*>(retro_atomic_exchange_ptr(&m_devices, nullptr)))
    FreeDevices(&devices->node);
  delete static_cast<Clients*>(retro_atomic_exchange_ptr(&m_clients, nullptr));
  quiesce_deinit(&m_quiesce);
}

void USBScanner::FreeDevices(quiesce_node_t* node)
{
  Devices* const devices = reinterpret_cast<Devices*>(node);
  usb_device_list_clear(&devices->list);
  delete devices;
}

void USBScanner::FreeClients(quiesce_node_t* node)
{
  delete reinterpret_cast<Clients*>(node);
}

void USBScanner::PublishDevices(USBDeviceList* list)
{
  Devices* const next = new Devices;
  next->node = {};
  memcpy(&next->list.ids, list->ids, list->count * sizeof(list->ids[0]));
  memcpy(&next->list.devices, list->devices, list->count * sizeof(list->devices[0]));
  next->list.count = list->count;
  list->count = 0;
  if (void* old = retro_atomic_exchange_ptr(&m_devices, next))
    quiesce_retire(&m_quiesce, &static_cast<Devices*>(old)->node, FreeDevices);
}

#if defined(__LIBUSB__)
void USBScanner::Shutdown()
{
  if(m_context)
    m_context.Shutdown();
}
#endif

void USBScanner::WaitForFirstScan()
{
  if (m_thread_running.IsSet())
  {
    m_first_scan_complete_flag.Wait(true);
  }
}

// Clients come and go on the emulation thread only, so each change is a plain swap.
bool USBScanner::AddClient(USBHost* client)
{
  const auto* cur = static_cast<const Clients*>(retro_atomic_load_acquire_ptr(&m_clients));
  const u32 count = cur ? cur->count : 0;
  u32 slot = 0;
  while (slot < count &&
         reinterpret_cast<uintptr_t>(cur->hosts[slot]) < reinterpret_cast<uintptr_t>(client))
    slot++;
  if (slot < count && cur->hosts[slot] == client)
    return false;
  if (count == MAX_CLIENTS)
  {
    ERROR_LOG(IOS_USB, "Too many USB scanner clients");
    return false;
  }

  auto* next = new Clients{};
  for (u32 i = 0; i < slot; i++)
    next->hosts[i] = cur->hosts[i];
  next->hosts[slot] = client;
  for (u32 i = slot; i < count; i++)
    next->hosts[i + 1] = cur->hosts[i];
  next->count = count + 1;
  const bool should_start_scanning = count == 0;
  if (void* old = retro_atomic_exchange_ptr(&m_clients, next))
    quiesce_retire(&m_quiesce, &static_cast<Clients*>(old)->node, FreeClients);

  if (should_start_scanning)
    StartScanning();

  return true;
}

bool USBScanner::RemoveClient(USBHost* client)
{
  const auto* cur = static_cast<const Clients*>(retro_atomic_load_acquire_ptr(&m_clients));
  u32 slot = 0;
  while (cur && slot < cur->count && cur->hosts[slot] != client)
    slot++;
  if (!cur || slot == cur->count)
    return false;

  auto* next = new Clients{};
  for (u32 i = 0, j = 0; i < cur->count; i++)
  {
    if (i != slot)
      next->hosts[j++] = cur->hosts[i];
  }
  next->count = cur->count - 1;
  const bool should_stop_scanning = next->count == 0;
  if (void* old = retro_atomic_exchange_ptr(&m_clients, next))
    quiesce_retire(&m_quiesce, &static_cast<Clients*>(old)->node, FreeClients);

  // The scan thread may be calling this client from the old set; let it finish.
  call_gate_close(&m_notify_gate);
  call_gate_reopen(&m_notify_gate);

  if (should_stop_scanning)
    StopScanning();

  return true;
}

void USBScanner::StartScanning()
{
  if (m_thread_running.TestAndSet())
  {
    m_thread = sthread_create(Common::ThreadEntry<&USBScanner::ScanThread>, this);
  }
}

void USBScanner::ScanThread()
{
  Common::SetCurrentThreadName("USB Scan Thread");
  while (m_thread_running.IsSet())
  {
    if (UpdateDevices())
      m_first_scan_complete_flag.Set(true);
    if (m_notify_pending)
      NotifyClients();
    // Rescan every few frames, about 50 ms.
    frame_clock_wait_until(frame_clock_now() + 3, m_thread_running.Raw());
  }
  USBDeviceList none;
  none.count = 0;
  PublishDevices(&none);
  m_first_scan_complete_flag.Set(false);
}

void USBScanner::StopScanning()
{
  if (m_thread_running.TestAndClear())
  {
    frame_clock_wake_all();
    Common::JoinThread(m_thread);
  }
}

void USBScanner::GetDevices(USBDeviceList* out) const
{
  quiesce_enter(&m_quiesce);
  const auto* devices = static_cast<const Devices*>(retro_atomic_load_acquire_ptr(&m_devices));
  if (devices)
    usb_device_list_copy(out, &devices->list);
  else
    out->count = 0;
  quiesce_exit(&m_quiesce);
}

// This is called from the scan thread. Returns false if we failed to update the device list.
bool USBScanner::UpdateDevices()
{
  USBDeviceList new_devices;
  new_devices.count = 0;
  if (!AddNewDevices(&new_devices))
  {
    usb_device_list_clear(&new_devices);
    return false;
  }

  // Only this thread replaces the list, so the current one stays valid here.
  const auto* devices = static_cast<const Devices*>(retro_atomic_load_acquire_ptr(&m_devices));
  const u32 current_count = devices ? devices->list.count : 0;
  if (current_count != new_devices.count ||
      (current_count != 0 &&
       memcmp(devices->list.ids, new_devices.ids, current_count * sizeof(new_devices.ids[0])) != 0))
  {
    PublishDevices(&new_devices);
    NotifyClients();
  }
  else
  {
    usb_device_list_clear(&new_devices);
  }

  return true;
}

void USBScanner::NotifyClients()
{
  call_gate_frame_t frame;
  if (!call_gate_enter(&m_notify_gate, &frame))
  {
    m_notify_pending = true;
    return;
  }
  m_notify_pending = false;

  quiesce_enter(&m_quiesce);
  const auto* devices = static_cast<const Devices*>(retro_atomic_load_acquire_ptr(&m_devices));
  const auto* clients = static_cast<const Clients*>(retro_atomic_load_acquire_ptr(&m_clients));
  if (devices && clients)
  {
    for (u32 i = 0; i < clients->count; i++)
      clients->hosts[i]->OnDevicesChanged(&devices->list);
  }
  quiesce_exit(&m_quiesce);

  call_gate_exit(&m_notify_gate, &frame);
}

bool USBScanner::AddNewDevices(USBDeviceList* new_devices) const
{
  AddEmulatedDevices(new_devices);
#ifdef __LIBUSB__
  if (!Core::WantsDeterminism())
  {
    const std::string whitelist = Config::Get(Config::MAIN_USB_PASSTHROUGH_DEVICES);
    if (!Config::USBDeviceListHas(whitelist.c_str(), nullptr))
      return true;

    if (m_context.IsValid())
    {
      struct Scan
      {
        const char* whitelist;
        USBDeviceList* new_devices;
      };
      Scan scan{whitelist.c_str(), new_devices};
      const int ret = m_context.GetDeviceList(
          [](void* user, libusb_device* device) -> bool {
            const Scan* const scan_state = static_cast<const Scan*>(user);
            libusb_device_descriptor descriptor;
            libusb_get_device_descriptor(device, &descriptor);
            if (descriptor.idVendor == 0x1209 && descriptor.idProduct == 0x2882)
            {
              WakeupSantrollerDevice(device);
            }
            const USBUtils::DeviceInfo device_info{descriptor.idVendor, descriptor.idProduct};
            if (!Config::USBDeviceListHas(scan_state->whitelist, &device_info))
              return true;

            usb_device_list_add(scan_state->new_devices, new USB::LibusbDevice(device, descriptor));
            return true;
          },
          &scan);
      if (ret != LIBUSB_SUCCESS)
        WARN_LOG(IOS_USB, "GetDeviceList failed: %s (%d: %s)",
                 LibusbUtils::ErrorWrap(ret).GetStrError(), LibusbUtils::ErrorWrap(ret).m_error,
                 LibusbUtils::ErrorWrap(ret).GetName());
    }
  }
#endif
  return true;
}

void USBScanner::AddEmulatedDevices(USBDeviceList* new_devices)
{
  if (Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL))
  {
    usb_device_list_add(new_devices, new USB::SkylanderUSB());
  }
  if (Config::Get(Config::MAIN_EMULATE_INFINITY_BASE))
  {
    usb_device_list_add(new_devices, new USB::InfinityUSB());
  }
  if (Config::Get(Config::MAIN_EMULATE_WII_SPEAK))
  {
    usb_device_list_add(new_devices, new USB::WiiSpeak());
  }
  for (u8 index = 0; index != Config::EMULATED_LOGITECH_MIC_COUNT; ++index)
  {
    if (Config::Get(Config::MAIN_EMULATE_LOGITECH_MIC[index]))
    {
      usb_device_list_add(new_devices, new USB::LogitechMic(index));
    }
  }
}

void USBScanner::WakeupSantrollerDevice(libusb_device* device)
{
#ifdef __LIBUSB__
  // Santroller devices emulate various instruments for multiple consoles.
  // On an actual console, santroller detects the console based on how it communicates
  // with usb devices. Since the underlying operating system is doing that here, the
  // check doesn't work, so we need to send a special command to make the device
  // jump to wii emulation mode.
  libusb_device_handle* lusb_handle;
  if (libusb_open(device, &lusb_handle) == LIBUSB_SUCCESS)
  {
#ifdef __linux__
    libusb_set_auto_detach_kernel_driver(lusb_handle, true);
    libusb_claim_interface(lusb_handle, 2);
#endif
    libusb_control_transfer(
        lusb_handle, +LIBUSB_ENDPOINT_IN | +LIBUSB_REQUEST_TYPE_CLASS | +LIBUSB_RECIPIENT_INTERFACE,
        0x01, 0x03f2, 2, nullptr, 0x11, 5000);
    libusb_close(lusb_handle);
  }
#endif
}

}  // namespace IOS::HLE
