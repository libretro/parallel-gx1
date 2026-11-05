// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/USBScanner.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <set>
#include <utility>

#ifdef __LIBUSB__
#include <libusb.h>
#endif

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
USBScanner::~USBScanner()
{
  StopScanning();
  delete static_cast<Devices*>(retro_atomic_exchange_ptr(&m_devices, nullptr));
  delete static_cast<Clients*>(retro_atomic_exchange_ptr(&m_clients, nullptr));
  quiesce_deinit(&m_quiesce);
}

void USBScanner::FreeDevices(quiesce_node_t* node)
{
  delete reinterpret_cast<Devices*>(node);
}

void USBScanner::FreeClients(quiesce_node_t* node)
{
  delete reinterpret_cast<Clients*>(node);
}

void USBScanner::PublishDevices(DeviceMap map)
{
  auto* next = new Devices{{}, std::move(map)};
  if (void* old = retro_atomic_exchange_ptr(&m_devices, next))
    quiesce_retire(&m_quiesce, &static_cast<Devices*>(old)->node, FreeDevices);
}

#if defined(__LIBRETRO__) && defined(__LIBUSB__)
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
  auto* next = new Clients{{}, cur ? cur->set : std::set<USBHost*>{}};
  const bool should_start_scanning = next->set.empty();
  const bool client_is_new = next->set.insert(client).second;
  if (void* old = retro_atomic_exchange_ptr(&m_clients, next))
    quiesce_retire(&m_quiesce, &static_cast<Clients*>(old)->node, FreeClients);

  if (should_start_scanning)
    StartScanning();

  return client_is_new;
}

bool USBScanner::RemoveClient(USBHost* client)
{
  const auto* cur = static_cast<const Clients*>(retro_atomic_load_acquire_ptr(&m_clients));
  if (!cur || !cur->set.contains(client))
    return false;

  auto* next = new Clients{{}, cur->set};
  next->set.erase(client);
  const bool should_stop_scanning = next->set.empty();
  if (void* old = retro_atomic_exchange_ptr(&m_clients, next))
    quiesce_retire(&m_quiesce, &static_cast<Clients*>(old)->node, FreeClients);

  // The scan thread may be calling this client from the old set; let it finish.
  Common::CallGateClose(&m_notify_gate);
  Common::CallGateReopen(&m_notify_gate);

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
    Common::SleepCurrentThread(50);
  }
  PublishDevices({});
  m_first_scan_complete_flag.Set(false);
}

void USBScanner::StopScanning()
{
  if (m_thread_running.TestAndClear())
    Common::JoinThread(m_thread);
}

USBScanner::DeviceMap USBScanner::GetDevices() const
{
  quiesce_enter(&m_quiesce);
  const auto* devices = static_cast<const Devices*>(retro_atomic_load_acquire_ptr(&m_devices));
  DeviceMap copy = devices ? devices->map : DeviceMap{};
  quiesce_exit(&m_quiesce);
  return copy;
}

// This is called from the scan thread. Returns false if we failed to update the device list.
bool USBScanner::UpdateDevices()
{
  DeviceMap new_devices;
  if (!AddNewDevices(&new_devices))
    return false;

  // Only this thread replaces the list, so the current one stays valid here.
  const auto* devices = static_cast<const Devices*>(retro_atomic_load_acquire_ptr(&m_devices));
  static const DeviceMap s_none;
  const DeviceMap& current = devices ? devices->map : s_none;
  if (!std::ranges::equal(std::views::keys(current), std::views::keys(new_devices)))
  {
    PublishDevices(std::move(new_devices));
    NotifyClients();
  }

  return true;
}

void USBScanner::NotifyClients()
{
  if (!Common::CallGateEnter(&m_notify_gate))
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
    for (USBHost* client : clients->set)
      client->OnDevicesChanged(devices->map);
  }
  quiesce_exit(&m_quiesce);

  Common::CallGateExit(&m_notify_gate);
}

bool USBScanner::AddNewDevices(DeviceMap* new_devices) const
{
  AddEmulatedDevices(new_devices);
#ifdef __LIBUSB__
  if (!Core::WantsDeterminism())
  {
    const auto whitelist = Config::GetUSBDeviceWhitelist();
    if (whitelist.empty())
      return true;

    if (m_context.IsValid())
    {
      const int ret = m_context.GetDeviceList([&](libusb_device* device) {
        libusb_device_descriptor descriptor;
        libusb_get_device_descriptor(device, &descriptor);
        if (descriptor.idVendor == 0x1209 && descriptor.idProduct == 0x2882)
        {
          WakeupSantrollerDevice(device);
        }
        const USBUtils::DeviceInfo device_info{descriptor.idVendor, descriptor.idProduct};
        if (!whitelist.contains(device_info))
          return true;

        auto usb_device = std::make_unique<USB::LibusbDevice>(device, descriptor);
        AddDevice(std::move(usb_device), new_devices);
        return true;
      });
      if (ret != LIBUSB_SUCCESS)
        WARN_LOG_FMT(IOS_USB, "GetDeviceList failed: {}", LibusbUtils::ErrorWrap(ret));
    }
  }
#endif
  return true;
}

void USBScanner::AddEmulatedDevices(DeviceMap* new_devices)
{
  if (Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL))
  {
    auto skylanderportal = std::make_unique<USB::SkylanderUSB>();
    AddDevice(std::move(skylanderportal), new_devices);
  }
  if (Config::Get(Config::MAIN_EMULATE_INFINITY_BASE))
  {
    auto infinity_base = std::make_unique<USB::InfinityUSB>();
    AddDevice(std::move(infinity_base), new_devices);
  }
  if (Config::Get(Config::MAIN_EMULATE_WII_SPEAK))
  {
    auto wii_speak = std::make_unique<USB::WiiSpeak>();
    AddDevice(std::move(wii_speak), new_devices);
  }
  for (u8 index = 0; index != Config::EMULATED_LOGITECH_MIC_COUNT; ++index)
  {
    if (Config::Get(Config::MAIN_EMULATE_LOGITECH_MIC[index]))
    {
      auto logitech_mic = std::make_unique<USB::LogitechMic>(index);
      AddDevice(std::move(logitech_mic), new_devices);
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

void USBScanner::AddDevice(std::unique_ptr<USB::Device> device, DeviceMap* new_devices)
{
  (*new_devices)[device->GetId()] = std::move(device);
}

}  // namespace IOS::HLE
