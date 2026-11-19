// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

namespace WiimoteEmu
{
struct DesiredWiimoteState
{
  // 1g in Z direction, which is the default returned by an unmoving emulated Wiimote.
  static constexpr WiimoteCommon::AccelData DEFAULT_ACCELERATION = WiimoteCommon::AccelData(
      {Wiimote::ACCEL_ZERO_G << 2, Wiimote::ACCEL_ZERO_G << 2, Wiimote::ACCEL_ONE_G << 2});

  WiimoteCommon::ButtonData buttons{};  // non-button state in this is ignored
  WiimoteCommon::AccelData acceleration = DEFAULT_ACCELERATION;
  // No light detected by the IR camera unless set: a CameraPoint is "not visible" when made.
  CameraPoint camera_points[CameraLogic::NUM_POINTS];
  std::optional<MotionPlus::DataFormat::Data> motion_plus = std::nullopt;
  DesiredExtensionState extension;
};

}  // namespace WiimoteEmu
