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

}  // namespace USBUtils
