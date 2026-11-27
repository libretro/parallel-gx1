// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/OH0/OH0Device.h"

#include <cstdlib>

#include "Common/ChunkFile.h"
#include "Common/StringUtil.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/OH0/OH0.h"

namespace IOS::HLE
{
// From "/dev/usb/oh0/<vid>/<pid>", its five non-empty '/'-separated parts; left alone otherwise
static void GetVidPidFromDevicePath(const char* device_path, u16& vid, u16& pid)
{
  const char* parts[5];
  u32 count = 0;
  const char* c = device_path;
  while (*c)
  {
    if (*c == '/')
    {
      c++;
      continue;
    }
    if (count == 5)
      return;
    parts[count++] = c;
    while (*c && *c != '/')
      c++;
  }

  if (count != 5)
    return;

  vid = static_cast<u16>(strtoul(parts[3], nullptr, 16));
  pid = static_cast<u16>(strtoul(parts[4], nullptr, 16));
}

OH0Device::OH0Device(EmulationKernel& ios, const char* name)
    : EmulationDevice(ios, name, DeviceType::OH0)
{
  if (*name)
    GetVidPidFromDevicePath(name, m_vid, m_pid);
}

void OH0Device::DoState(PointerWrap& p)
{
  m_oh0 = static_cast<OH0*>(GetEmulationKernel().GetDeviceByName("/dev/usb/oh0"));
  Device::DoState(p);
  p.Do(m_vid);
  p.Do(m_pid);
  p.Do(m_device_id);
}

IPCReply OH0Device::Open(const OpenRequest& request)
{
  if (m_vid == 0 && m_pid == 0)
    return IPCReply(IPC_ENOENT);

  m_oh0 = static_cast<OH0*>(GetEmulationKernel().GetDeviceByName("/dev/usb/oh0"));

  return IPCReply(m_oh0->DeviceOpen(m_vid, m_pid, &m_device_id));
}

IPCReply OH0Device::Close(u32 fd)
{
  m_oh0->DeviceClose(m_device_id);
  return Device::Close(fd);
}

IPCReply OH0Device::IOCtl(const IOCtlRequest& request)
{
  return m_oh0->DeviceIOCtl(m_device_id, request);
}

IPCReply OH0Device::IOCtlV(const IOCtlVRequest& request)
{
  return m_oh0->DeviceIOCtlV(m_device_id, request);
}
}  // namespace IOS::HLE
