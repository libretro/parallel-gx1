// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/SerialDevice.h"

namespace Triforce
{

// FFB wheel used by MarioKartGP and MarioKartGP2.
class MarioKartGPSteeringWheel final : public SerialDevice
{
public:
  void Update() override;

  void Reset();

  void DoState(PointerWrap&) override;

private:
  static constexpr std::size_t REQUEST_SIZE = 10;

  void ProcessRequest(const u8* request);

  u8 m_init_state = 0;
};

// Used for both MarioKartGP and MarioKartGP2.
class MarioKartGPCommon_IOAdapter final : public IOAdapter
{
public:
  explicit MarioKartGPCommon_IOAdapter(MarioKartGPSteeringWheel* steering_wheel)
      : m_steering_wheel{steering_wheel}
  {
  }

  void Update() override;

protected:
  void HandleGenericOutputsChanged(const u8* bits_set, const u8* bits_cleared) override;

private:
  MarioKartGPSteeringWheel* const m_steering_wheel;
};

}  // namespace Triforce
