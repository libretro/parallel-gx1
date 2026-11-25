// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/IOS/Device.h"

namespace IOS::HLE
{
class AesDevice final : public EmulationDevice
{
public:
  AesDevice(EmulationKernel& ios, const std::string& device_name);
  IPCReply Open(const OpenRequest& request) override;
  ~AesDevice() override;
  IPCReply IOCtlV(const IOCtlVRequest& request) override;

  enum class AesIoctlv : u32
  {
    Copy = 0x00,
    Encrypt = 0x02,
    Decrypt = 0x03,
  };

private:
  // Request data, grown by doubling and kept across requests
  u8* m_buffer = nullptr;
  size_t m_capacity = 0;
  u8* Buffer(size_t size);
};
}  // namespace IOS::HLE
