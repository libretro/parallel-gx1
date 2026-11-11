// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/HSP/HSP.h"

#include <memory>

#include <cstdio>

#include "Common/ChunkFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/HSP/HSP_Device.h"

namespace HSP
{
HSPManager::HSPManager() = default;
HSPManager::~HSPManager() = default;

void HSPManager::Init()
{
  SetDevice(Config::Get(Config::MAIN_HSP_DEVICE));
}

void HSPManager::Shutdown()
{
  m_device.reset();
}

void HSPManager::Read(u32 address, u8* data)
{
  DEBUG_LOG(HSP, "HSP read from 0x%08x", address);

  m_device->Read(address, data);
}

void HSPManager::Write(u32 address, const u8* data)
{
#if defined(_DEBUG) || defined(DEBUGFAST)
  char bytes[TRANSFER_SIZE * 3];
  for (std::size_t i = 0; i < TRANSFER_SIZE; i++)
    snprintf(bytes + i * 3, sizeof(bytes) - i * 3, i ? " %02x" : "%02x", data[i]);
  DEBUG_LOG(HSP, "HSP write to 0x%08x: %s", address, bytes);
#endif

  m_device->Write(address, data);
}

void HSPManager::DoState(PointerWrap& p)
{
  const HSPDeviceType current_type = m_device->GetDeviceType();
  auto state_type = current_type;
  p.Do(state_type);

  // If the type doesn't match, switch to the right device type
  if (state_type != current_type)
    SetDevice(state_type);

  m_device->DoState(p);
}

void HSPManager::SetDevice(std::unique_ptr<IHSPDevice> device)
{
  m_device = std::move(device);
}

void HSPManager::SetDevice(const HSPDeviceType device)
{
  SetDevice(HSPDevice_Create(device));
}

}  // namespace HSP
