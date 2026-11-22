// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <rthreads/rthreads.h>

#include <retro_atomic.h>

#include "Common/CallGate.h"
#include "Common/Quiesce.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/WaitableFlag.h"
#include "Core/IOS/USB/Common.h"
#include "Core/LibusbUtils.h"

class PointerWrap;

namespace IOS::HLE
{
class USBHost;

// Devices sorted by ID, the list holding a reference on each
struct USBDeviceList
{
  static constexpr u32 MAX_DEVICES = 64;
  u32 count;
  u64 ids[MAX_DEVICES];
  USB::Device* devices[MAX_DEVICES];
};
void usb_device_list_clear(USBDeviceList* list);
// dst starts empty
void usb_device_list_copy(USBDeviceList* dst, const USBDeviceList* src);
USB::Device* usb_device_list_find(const USBDeviceList* list, u64 id);
// Takes the reference held on device, replacing any device with its ID
void usb_device_list_add(USBDeviceList* list, USB::Device* device);
// Returns the device with the list's reference on it
USB::Device* usb_device_list_take(USBDeviceList* list, u32 index);

class USBScanner final
{
public:
  ~USBScanner();

  bool AddClient(USBHost* client);
  bool RemoveClient(USBHost* client);

  void WaitForFirstScan();

  // out starts empty
  void GetDevices(USBDeviceList* out) const;

#if defined(__LIBUSB__)
  void Shutdown();
#endif

private:
  void StartScanning();
  void StopScanning();

  bool UpdateDevices();
  void ScanThread();
  bool AddNewDevices(USBDeviceList* new_devices) const;
  static void WakeupSantrollerDevice(libusb_device* device);
  static void AddEmulatedDevices(USBDeviceList* new_devices);

  // The device list and the clients are each published whole: the scan thread replaces the
  // list, the emulation thread (the only one adding and removing clients) replaces the set.
  // Old copies go once no reader is inside m_quiesce.
  struct Devices
  {
    quiesce_node_t node;  // first: the free callback casts it back
    USBDeviceList list;
  };
  // Sorted by address, the order the hosts are told in
  static constexpr u32 MAX_CLIENTS = 16;
  struct Clients
  {
    quiesce_node_t node;  // first, as above
    u32 count;
    USBHost* hosts[MAX_CLIENTS];
  };
  static void FreeDevices(quiesce_node_t* node);
  static void FreeClients(quiesce_node_t* node);
  // Takes the devices in list, leaving it empty
  void PublishDevices(USBDeviceList* list);
  // Tells the clients about the current devices. Scan thread.
  void NotifyClients();

  mutable quiesce_t m_quiesce{};
  retro_atomic_ptr_t m_devices{};
  retro_atomic_ptr_t m_clients{};
  // The scan thread is inside it while calling clients, so RemoveClient can wait that out
  // before its caller goes away.
  call_gate_t m_notify_gate{};
  // A notification skipped while a removal had the gate shut; the next scan sends it.
  bool m_notify_pending = false;

  Common::Flag m_thread_running;
  sthread_t* m_thread = nullptr;
  Common::WaitableFlag m_first_scan_complete_flag;

  LibusbUtils::Context m_context;
};

}  // namespace IOS::HLE
