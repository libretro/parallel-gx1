// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteReal/IOhidapi.h"

#include <cassert>

#if defined(__linux__)
#endif

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

using namespace WiimoteCommon;
using namespace WiimoteReal;

static bool IsDeviceUsable(const std::string& device_path)
{
  hid_device* handle = hid_open_path(device_path.c_str());
  if (handle == nullptr)
  {
    ERROR_LOG(WIIMOTE,
              "Could not connect to Wii Remote at \"%s\". "
              "Do you have permission to access the device?",
              device_path.c_str());
    return false;
  }
  // Some third-party adapters (DolphinBar) always expose all four Wii Remotes as HIDs
  // even when they are not connected, which causes an endless error loop when we try to use them.
  // Try to write a report to the device to see if this Wii Remote is really usable.
  static const u8 report[] = {u8(OutputReportID::RequestStatus), 0};
  const int result = hid_write(handle, report, sizeof(report));
  // The DolphinBar uses EPIPE to signal the absence of a Wii Remote connected to this HID.
  if (result == -1 && errno != EPIPE)
    ERROR_LOG(WIIMOTE, "Couldn't write to Wii Remote at \"%s\".", device_path.c_str());

  hid_close(handle);
  return result != -1;
}

namespace WiimoteReal
{
WiimoteScannerHidapi::WiimoteScannerHidapi()
{
#ifdef __OpenBSD__
  // OpenBSD renamed libhidapi's hidapi_init function because the system has its own USB library
  // which contains a symbol with the same name. See:
  // https://cvsweb.openbsd.org/ports/comms/libhidapi/patches/patch-hidapi_hidapi_h?rev=1.3&content-type=text/x-cvsweb-markup
  // https://man.openbsd.org/usbhid.3
#define hid_init hidapi_hid_init
#endif
  int ret = hid_init();
  ASSERT_MSG(WIIMOTE, ret == 0, "Couldn't initialise hidapi.");
}

WiimoteScannerHidapi::~WiimoteScannerHidapi()
{
  if (hid_exit() == -1)
    ERROR_LOG(WIIMOTE, "Failed to clean up hidapi.");
}

auto WiimoteScannerHidapi::FindAttachedWiimotes() -> FindResults
{
  FindResults results;

  hid_device_info* list = hid_enumerate(0x0, 0x0);  // FYI: 0 for all VID/PID.
  for (hid_device_info* device = list; device != nullptr; device = device->next)
  {
    const std::string name = device->product_string ? WStringToUTF8(device->product_string) : "";
    const bool is_wiimote =
        IsValidDeviceName(name) || IsKnownDeviceId({device->vendor_id, device->product_id});
    if (!is_wiimote || !IsNewWiimote(device->path) || !IsDeviceUsable(device->path))
      continue;

    auto wiimote = std::make_unique<WiimoteHidapi>(device->path);
    const bool is_balance_board = IsBalanceBoardName(name) || wiimote->IsBalanceBoard();
    if (is_balance_board)
      results.balance_boards.emplace_back(std::move(wiimote));
    else
      results.wii_remotes.emplace_back(std::move(wiimote));

    NOTICE_LOG(WIIMOTE, "Found %s at %s: %s %s (%04x:%04x)",
               is_balance_board ? "balance board" : "Wiimote", device->path,
               WStringToUTF8(device->manufacturer_string).c_str(),
               WStringToUTF8(device->product_string).c_str(), (unsigned)device->vendor_id,
               (unsigned)device->product_id);
  }
  hid_free_enumeration(list);

  return results;
}

WiimoteHidapi::WiimoteHidapi(std::string device_path) : m_device_path(std::move(device_path))
{
}

WiimoteHidapi::~WiimoteHidapi()
{
  Shutdown();
}

bool WiimoteHidapi::ConnectInternal()
{
  if (m_handle != nullptr)
    return true;

  m_handle = hid_open_path(m_device_path.c_str());
  if (m_handle == nullptr)
  {
    ERROR_LOG(WIIMOTE,
              "Could not connect to Wii Remote at \"%s\". "
              "Do you have permission to access the device?",
              m_device_path.c_str());
  }
  return m_handle != nullptr;
}

void WiimoteHidapi::DisconnectInternal()
{
  hid_close(m_handle);
  m_handle = nullptr;
}

bool WiimoteHidapi::IsConnected() const
{
  return m_handle != nullptr;
}

int WiimoteHidapi::IORead(u8* buf)
{
  int timeout = 200;  // ms
  int result = hid_read_timeout(m_handle, buf + 1, MAX_PAYLOAD - 1, timeout);
  // TODO: If and once we use hidapi across platforms, change our internal API to clean up this
  // mess.
  if (result == -1)
  {
    ERROR_LOG(WIIMOTE, "Failed to read from %s.", m_device_path.c_str());
    return 0;  // error
  }
  if (result == 0)
  {
    return -1;  // didn't read packet
  }
  buf[0] = WR_SET_REPORT | BT_INPUT;
  return result + 1;  // number of bytes read
}

int WiimoteHidapi::IOWrite(const u8* buf, size_t len)
{
  assert(len > 0);
  DEBUG_ASSERT(buf[0] == (WR_SET_REPORT | BT_OUTPUT));
  int result = hid_write(m_handle, buf + 1, len - 1);
  if (result == -1)
  {
    ERROR_LOG(WIIMOTE, "Failed to write to %s.", m_device_path.c_str());
    return 0;
  }
  return (result == 0) ? 1 : result;
}
};  // namespace WiimoteReal
