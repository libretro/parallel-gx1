// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/HSP/HSP_Device.h"

namespace HSP
{
class CHSPDevice_Null final : public IHSPDevice
{
public:
  HSPDeviceType GetDeviceType() const override { return HSPDeviceType::None; }

  void Read(u32 address, u8* data) override;
  void Write(u32 address, const u8* data) override;
};
}  // namespace HSP
