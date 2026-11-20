// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/HSP/HSP_Device.h"

#include <memory>

#include "Core/HW/HSP/HSP_DeviceARAMExpansion.h"
#include "Core/HW/HSP/HSP_DeviceGBPlayer.h"
#include "Core/HW/HSP/HSP_DeviceNull.h"
#include "Core/System.h"

namespace HSP
{
IHSPDevice::~IHSPDevice() = default;

void IHSPDevice::DoState(PointerWrap& p)
{
}

IHSPDevice* HSPDevice_Create(const HSPDeviceType device)
{
  auto& system = Core::System::GetInstance();

  switch (device)
  {
  case HSPDeviceType::ARAMExpansion:
    return new CHSPDevice_ARAMExpansion();
  case HSPDeviceType::GBPlayer:
    return new CHSPDevice_GBPlayer(system);
  case HSPDeviceType::None:
  default:
    return new CHSPDevice_Null();
  }
}
}  // namespace HSP
