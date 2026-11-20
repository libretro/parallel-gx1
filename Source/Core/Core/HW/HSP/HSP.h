// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/HSP/HSP_Device.h"

class PointerWrap;

namespace HSP
{
class HSPManager
{
public:
  HSPManager();
  HSPManager(const HSPManager& other) = delete;
  HSPManager(HSPManager&& other) = delete;
  HSPManager& operator=(const HSPManager& other) = delete;
  HSPManager& operator=(HSPManager&& other) = delete;
  ~HSPManager();

  void Init();
  void Shutdown();

  void Read(u32 address, u8* data);
  void Write(u32 address, const u8* data);

  void DoState(PointerWrap& p);

  void SetDevice(HSPDeviceType device);

private:
  // the device on the port, made by SetDevice and deleted when replaced or at Shutdown
  IHSPDevice* m_device = nullptr;
};
}  // namespace HSP
