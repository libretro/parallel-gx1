// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Host.h"

class PointerWrap;

namespace IOS::HLE
{
class USB_HIDv4 final : public USBHost
{
public:
  USB_HIDv4(EmulationKernel& ios, const std::string& device_name);
  ~USB_HIDv4() override;

  IPCReply IOCtl(const IOCtlRequest& request) override;

  void DoState(PointerWrap& p) override;

private:
  USB::Device* GetDeviceByIOSID(s32 ios_id) const;

  IPCReply CancelInterrupt(const IOCtlRequest& request);
  IPCReply GetDeviceChange(const IOCtlRequest& request);
  IPCReply Shutdown(const IOCtlRequest& request);
  s32 SubmitTransfer(USB::Device& device, const IOCtlRequest& request);

  void TriggerDeviceChangeReply();
  // Builds the device's entry into out; 0 when it does not fit
  static constexpr size_t MAX_DEVICE_ENTRY_SIZE = 2048;
  size_t GetDeviceEntry(const USB::Device& device, u8* out, size_t capacity) const;
  void OnDeviceChange(ChangeEvent, USB::Device*) override;
  bool ShouldAddDevice(const USB::Device& device) const override;

  static constexpr u32 VERSION = 0x40001;
  static constexpr u8 HID_CLASS = 0x03;

  bool m_has_pending_changes = true;
  bool m_is_shut_down = false;
  // The pending device change request's address, 0 for none
  u32 m_devicechange_hook_address = 0;
  // IOS device IDs <=> USB device IDs, sorted by IOS ID
  struct DeviceIds
  {
    s32 ios_id;
    u64 device_id;
  };
  static constexpr u32 MAX_DEVICES = 64;
  DeviceIds m_ids[MAX_DEVICES]{};
  u32 m_id_count = 0;
  const DeviceIds* FindByDeviceId(u64 device_id) const;
};
}  // namespace IOS::HLE
