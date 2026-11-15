// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class AnalogStick : public ReshapableInput
{
  // the base branches to its reshaping calls, private ones included
  friend class ReshapableInput;

public:
  using StateData = ReshapeData;

  AnalogStick(const char* name, const StickGate& stick_gate);
  AnalogStick(const char* name, const char* ui_name, const StickGate& stick_gate);

protected:
  // For the octagonal stick built on this one
  AnalogStick(const char* name, const char* ui_name, const StickGate& stick_gate, ReshapeKind kind);

public:
  ReshapeData GetReshapableState(bool adjusted) const;
  ControlState GetGateRadiusAtAngle(double ang) const;

  StateData GetState() const;
  StateData GetState(const InputOverrideFunction& override_func) const;

private:
  Control* GetModifierInput() const;

  StickGate m_stick_gate;
};

// An AnalogStick with an OctagonStickGate
class OctagonAnalogStick : public AnalogStick
{
  // the base branches to its reshaping calls, private ones included
  friend class ReshapableInput;

public:
  OctagonAnalogStick(const char* name, ControlState gate_radius);
  OctagonAnalogStick(const char* name, const char* ui_name, ControlState gate_radius);

  ControlState GetVirtualNotchSize() const;
  ControlState GetGateRadiusAtAngle(double ang) const;

private:
  SettingValue<double> m_virtual_notch_setting;
  SettingValue<double> m_gate_size_setting;
};

}  // namespace ControllerEmu
