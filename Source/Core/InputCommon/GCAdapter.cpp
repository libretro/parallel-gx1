// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/GCAdapter.h"

#include "InputCommon/GCPadStatus.h"

// The libretro core reaches no USB adapter: the libusb and Android Java paths belonged to the
// standalone builds. A libretro frontend owns the controllers, so an adapter configured on a
// port simply never reports one connected.
namespace GCAdapter
{
void Init()
{
}

void Shutdown()
{
}

void SetAdapterCallback(std::function<void(void)> func)
{
}

GCPadStatus Input(int chan)
{
  return {};
}

bool DeviceConnected(int chan)
{
  return false;
}

void ResetDeviceType(int chan)
{
}

void ResetRumble()
{
}

void Output(int chan, u8 rumble_command)
{
}

bool IsDetected(const char** error_message)
{
  return false;
}

double GetCurrentPollRate()
{
  return 0.0;
}

}  // namespace GCAdapter
