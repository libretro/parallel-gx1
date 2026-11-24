// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/USBUtils.h"

#include <array>
#include <cwchar>
#include <functional>
#include <map>
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
std::string DeviceInfo::ToString() const
{
  return StringFromFormat("%04x:%04x", (unsigned)vid, (unsigned)pid);
}

}  // namespace USBUtils
