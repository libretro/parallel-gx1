// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <setupapi.h>

namespace Common
{
// Each false when the property is missing or not a string
bool GetDevNodeStringProperty(DEVINST device, const DEVPROPKEY* requested_property,
                              std::wstring* out);

bool GetDeviceInterfaceStringProperty(LPCWSTR iface, const DEVPROPKEY* requested_property,
                                      std::wstring* out);

// The device interfaces as a list of null-terminated strings, ended by an empty one;
// freed with std::free.
WCHAR* GetDeviceInterfaceList(LPGUID iface_class_guid, DEVINSTID device_id, ULONG flags);

class DeviceChangeNotification
{
public:
  enum class EventType : bool
  {
    Arrival,
    Removal,
  };
  typedef void (*CallbackType)(void* user, EventType event);
  struct Listener
  {
    CallbackType callback;
    void* user;
  };

  DeviceChangeNotification();
  ~DeviceChangeNotification();

  // FYI: Currently hardcoded to a GUID_DEVINTERFACE_HID filter.
  void Register(CallbackType callback, void* user);
  void Unregister();

  DeviceChangeNotification(const DeviceChangeNotification&) = delete;
  DeviceChangeNotification(DeviceChangeNotification&&) = delete;
  void operator=(const DeviceChangeNotification&) = delete;
  void operator=(DeviceChangeNotification&&) = delete;

private:
  Listener m_listener{};
  HCMNOTIFICATION m_notify_handle{nullptr};
};

}  // namespace Common

#endif
