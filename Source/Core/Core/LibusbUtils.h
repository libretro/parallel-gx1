// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"

struct libusb_config_descriptor;
struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace LibusbUtils
{
template <typename T>
using UniquePtr = std::unique_ptr<T, void (*)(T*)>;

// Return false to stop iterating the device list.
using GetDeviceListCallback = std::function<bool(libusb_device* device)>;

class Context
{
public:
  Context();
  ~Context();

  operator libusb_context*() const;
  bool IsValid() const;
#if defined(__LIBUSB__)
  void Shutdown();
#endif

  // Only valid if the context is valid.
  int GetDeviceList(const GetDeviceListCallback& callback) const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

using ConfigDescriptor = UniquePtr<libusb_config_descriptor>;
std::pair<int, ConfigDescriptor> MakeConfigDescriptor(libusb_device* device, u8 config_num = 0);

// A libusb error code with its description and name; most libusb functions return an int
// rather than a libusb_error.
struct ErrorWrap
{
  constexpr explicit ErrorWrap(int error) : m_error(error) {}
  const int m_error;

  const char* GetStrError() const;
  const char* GetName() const;
};

// Returns the UTF-16 descriptor converted to UTF-8 or returns nullopt on error.
std::optional<std::string> GetStringDescriptor(libusb_device_handle* dev_handle,
                                               uint8_t desc_index);

}  // namespace LibusbUtils
