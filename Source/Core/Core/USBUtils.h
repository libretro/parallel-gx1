// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

struct libusb_device_descriptor;

namespace USBUtils
{

struct DeviceInfo
{
  u16 vid;
  u16 pid;

  constexpr auto operator<=>(const DeviceInfo& other) const = default;

  // Extracts DeviceInfo from a string in the format "VID:PID"
  std::string ToString() const;
};

}  // namespace USBUtils
