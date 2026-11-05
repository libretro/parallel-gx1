// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <set>

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

class USBScanner final
{
public:
  using DeviceMap = std::map<u64, std::shared_ptr<USB::Device>>;

  ~USBScanner();

  bool AddClient(USBHost* client);
  bool RemoveClient(USBHost* client);

  void WaitForFirstScan();

  DeviceMap GetDevices() const;

#if defined(__LIBRETRO__) && defined(__LIBUSB__)
  void Shutdown();
#endif

private:
  void StartScanning();
  void StopScanning();

  bool UpdateDevices();
  void ScanThread();
  bool AddNewDevices(DeviceMap* new_devices) const;
  static void WakeupSantrollerDevice(libusb_device* device);
  static void AddEmulatedDevices(DeviceMap* new_devices);
  static void AddDevice(std::unique_ptr<USB::Device> device, DeviceMap* new_devices);

  // The device list and the clients are each published whole: the scan thread replaces the
  // list, the emulation thread (the only one adding and removing clients) replaces the set.
  // Old copies go once no reader is inside m_quiesce.
  struct Devices
  {
    quiesce_node_t node;  // first: the free callback casts it back
    DeviceMap map;
  };
  struct Clients
  {
    quiesce_node_t node;  // first, as above
    std::set<USBHost*> set;
  };
  static void FreeDevices(quiesce_node_t* node);
  static void FreeClients(quiesce_node_t* node);
  void PublishDevices(DeviceMap map);
  // Tells the clients about the current devices. Scan thread.
  void NotifyClients();

  mutable quiesce_t m_quiesce{};
  retro_atomic_ptr_t m_devices{};
  retro_atomic_ptr_t m_clients{};
  // The scan thread is inside it while calling clients, so RemoveClient can wait that out
  // before its caller goes away.
  Common::CallGate m_notify_gate;
  // A notification skipped while a removal had the gate shut; the next scan sends it.
  bool m_notify_pending = false;

  Common::Flag m_thread_running;
  sthread_t* m_thread = nullptr;
  Common::WaitableFlag m_first_scan_complete_flag;

  LibusbUtils::Context m_context;
};

}  // namespace IOS::HLE
