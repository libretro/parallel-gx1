// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Core/IOS/Device.h"

namespace IOS::HLE
{
class ShaDevice final : public EmulationDevice
{
public:
  ShaDevice(EmulationKernel& ios, const std::string& device_name);
  IPCReply Open(const OpenRequest& request) override;
  IPCReply IOCtlV(const IOCtlVRequest& request) override;

  enum class ShaIoctlv : u32
  {
    InitState = 0x00,
    ContributeState = 0x01,
    FinalizeState = 0x02,
    ShaCommandUnknown = 0x0F
  };

  struct ShaContext
  {
    u32 states[5];
    u32 length[2];  // length in bits of total data contributed to SHA-1 hash
  };

private:
  HLE::ReturnCode ProcessShaCommand(ShaIoctlv command, const IOCtlVRequest& request);
};
}  // namespace IOS::HLE
