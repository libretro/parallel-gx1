// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/USB/Host.h"

class PointerWrap;

namespace IOS::HLE
{
namespace USB
{
struct DeviceInfo
{
  u16 vid;
  u16 pid;
  bool operator<(const DeviceInfo& other) const { return vid < other.vid; }
};
}  // namespace USB

// /dev/usb/oh0
class OH0 final : public USBHost
{
public:
  OH0(EmulationKernel& ios, const std::string& device_name);
  ~OH0() override;

  IPCReply Open(const OpenRequest& request) override;
  IPCReply IOCtl(const IOCtlRequest& request) override;
  IPCReply IOCtlV(const IOCtlVRequest& request) override;

  std::pair<ReturnCode, u64> DeviceOpen(u16 vid, u16 pid);
  void DeviceClose(u64 device_id);
  IPCReply DeviceIOCtl(u64 device_id, const IOCtlRequest& request);
  IPCReply DeviceIOCtlV(u64 device_id, const IOCtlVRequest& request);

  void DoState(PointerWrap& p) override;

private:
  IPCReply CancelInsertionHook(const IOCtlRequest& request);
  IPCReply GetDeviceList(const IOCtlVRequest& request) const;
  IPCReply GetRhDesca(const IOCtlRequest& request) const;
  IPCReply GetRhPortStatus(const IOCtlVRequest& request) const;
  IPCReply SetRhPortStatus(const IOCtlVRequest& request);
  IPCReply RegisterRemovalHook(u64 device_id, const IOCtlRequest& request);
  IPCReply RegisterInsertionHook(const IOCtlVRequest& request);
  IPCReply RegisterInsertionHookWithID(const IOCtlVRequest& request);
  IPCReply RegisterClassChangeHook(const IOCtlVRequest& request);
  s32 SubmitTransfer(USB::Device& device, const IOCtlVRequest& request);

  bool HasDeviceWithVidPid(u16 vid, u16 pid) const;
  void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device) override;
  void TriggerInsertionHook(USB::DeviceInfo info, ReturnCode return_value);
  void TriggerRemovalHook(u64 device_id, ReturnCode return_value);
  void ReplyToHook(u32 address, ReturnCode return_value);
  bool IsDeviceOpened(u64 device_id) const;
  void SetDeviceOpened(u64 device_id, bool opened);

  struct DeviceEntry
  {
    u32 unknown;
    u16 vid;
    u16 pid;
  };
  static_assert(sizeof(DeviceEntry) == 8, "sizeof(DeviceEntry) must be 8");

  // The pending hook requests' command addresses, by device info (VID, PID) or device ID, and
  // the opened devices: each sorted by its key, as the maps and set they replace were saved
  static constexpr u32 MAX_ENTRIES = 32;
  struct InsertionHook
  {
    USB::DeviceInfo info;
    u32 address;
  };
  struct RemovalHook
  {
    u64 device_id;
    u32 address;
  };
  InsertionHook m_insertion_hooks[MAX_ENTRIES]{};
  u32 m_insertion_hook_count = 0;
  RemovalHook m_removal_hooks[MAX_ENTRIES]{};
  u32 m_removal_hook_count = 0;
  u64 m_opened_devices[MAX_ENTRIES]{};
  u32 m_opened_device_count = 0;
  void SetInsertionHook(USB::DeviceInfo info, u32 address, bool replace);
};
}  // namespace IOS::HLE
