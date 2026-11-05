// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <retro_atomic.h>
#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBScanner.h"

class PointerWrap;

namespace IOS::HLE
{
// Common base class for USB host devices (such as /dev/usb/oh0 and /dev/usb/ven).
class USBHost : public EmulationDevice
{
public:
  USBHost(EmulationKernel& ios, const std::string& device_name);
  ~USBHost() override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;

  void DoState(PointerWrap& p) override;

  void OnDevicesChanged(const USBScanner::DeviceMap& new_devices);

protected:
  enum class ChangeEvent
  {
    Inserted,
    Removed,
  };
  using DeviceChangeHooks = std::map<std::shared_ptr<USB::Device>, ChangeEvent>;

  std::shared_ptr<USB::Device> GetDeviceById(u64 device_id) const;
  virtual void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> changed_device);
  virtual void OnDeviceChangeEnd();
  virtual bool ShouldAddDevice(const USB::Device& device) const;

  std::optional<IPCReply> HandleTransfer(const std::shared_ptr<USB::Device>& device, u32 request,
                                         const std::function<s32()>& submit) const;

  // Emulation thread only. Device lists from the scan thread wait in m_pending_devices until
  // Update takes them, so this and everything derived from it in the subclasses stay on one
  // thread.
  std::map<u64, std::shared_ptr<USB::Device>> m_devices;

private:
  void Update() override;
  void OnDevicesChangedInternal(const USBScanner::DeviceMap& new_devices);

  bool m_has_initialised = false;
  retro_atomic_ptr_t m_pending_devices{};  // USBScanner::DeviceMap*
};
}  // namespace IOS::HLE
