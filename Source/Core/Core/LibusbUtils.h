// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

struct libusb_config_descriptor;
struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace LibusbUtils
{
// Return false to stop iterating the device list.
typedef bool (*GetDeviceListCallback)(void* user, libusb_device* device);

class Context
{
public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  operator libusb_context*() const;
  bool IsValid() const;
#if defined(__LIBUSB__)
  void Shutdown();
#endif

  // Only valid if the context is valid.
  int GetDeviceList(GetDeviceListCallback callback, void* user) const;

private:
  class Impl;
  Impl* m_impl;  // owned
};

// A libusb error code with its description and name; most libusb functions return an int
// rather than a libusb_error.
struct ErrorWrap
{
  constexpr explicit ErrorWrap(int error) : m_error(error) {}
  const int m_error;

  const char* GetStrError() const;
  const char* GetName() const;
};

// The UTF-16 descriptor converted to UTF-8; false on error.
bool GetStringDescriptor(libusb_device_handle* dev_handle, uint8_t desc_index, std::string* out);

}  // namespace LibusbUtils
