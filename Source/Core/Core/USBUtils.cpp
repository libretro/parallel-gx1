// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/USBUtils.h"

#include <array>
#include <charconv>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#ifdef __LIBUSB__
#include <libusb.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/LibusbUtils.h"

namespace USBUtils
{
std::optional<DeviceInfo> DeviceInfo::FromString(const std::string& str)
{
  const size_t colon_index = str.find(':');
  if (colon_index == std::string::npos)
    return std::nullopt;

  auto parse_hex = [](std::string_view sv, u16& out) -> bool {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out, 16);
    return ec == std::errc();
  };

  DeviceInfo info;
  std::string_view vid_sv(str.data(), colon_index);
  std::string_view pid_sv(str.data() + colon_index + 1);

  if (!parse_hex(vid_sv, info.vid))
    return std::nullopt;
  if (!parse_hex(pid_sv, info.pid))
    return std::nullopt;

  return info;
}

std::string DeviceInfo::ToString() const
{
  return StringFromFormat("%04x:%04x", (unsigned)vid, (unsigned)pid);
}

#ifdef __LIBUSB__
std::vector<DeviceInfo>
ListDevices(const std::function<bool(const libusb_device_descriptor&)>& filter)
{
  std::vector<DeviceInfo> device_list;
  LibusbUtils::Context context;
  if (!context.IsValid())
    return {};

  const int ret = context.GetDeviceList([&device_list, &filter](libusb_device* device) {
    libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(device, &desc) != LIBUSB_SUCCESS)
      return true;

    if (filter(desc))
    {
      device_list.push_back({desc.idVendor, desc.idProduct});
    }

    return true;
  });

  if (ret != LIBUSB_SUCCESS)
    WARN_LOG(CORE, "Failed to get device list: %s (%d: %s)",
             LibusbUtils::ErrorWrap(ret).GetStrError(), LibusbUtils::ErrorWrap(ret).m_error,
             LibusbUtils::ErrorWrap(ret).GetName());

  return device_list;
}

std::vector<DeviceInfo> ListDevices(const std::function<bool(const DeviceInfo&)>& filter)
{
  return ListDevices([&filter](const libusb_device_descriptor& desc) {
    const DeviceInfo info{desc.idVendor, desc.idProduct};
    return filter(info);
  });
}
#endif
}  // namespace USBUtils
