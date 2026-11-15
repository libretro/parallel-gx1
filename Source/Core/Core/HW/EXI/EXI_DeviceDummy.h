// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_Device.h"

namespace ExpansionInterface
{
// Just a dummy that logs reads and writes
// to be used for EXI devices we haven't emulated
// DOES NOT FUNCTION AS "NO DEVICE INSERTED" -> Appears as unknown device
class CEXIDummy final : public IEXIDevice
{
  // the base branches to its calls, private ones included
  friend class IEXIDevice;

public:
  CEXIDummy(Core::System& system, std::string name);

  void ImmWrite(u32 data, u32 size);
  u32 ImmRead(u32 size);

  void DMAWrite(u32 address, u32 size);
  void DMARead(u32 address, u32 size);

  bool IsPresent() const;

private:
  void TransferByte(u8& byte);

  std::string m_name;
};
}  // namespace ExpansionInterface
