// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"

#include <optional>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
AnalogStick::AnalogStick(const char* const name_, const StickGate& stick_gate)
    : AnalogStick(name_, name_, stick_gate)
{
}

AnalogStick::AnalogStick(const char* const name_, const char* const ui_name_,
                         const StickGate& stick_gate)
    : AnalogStick(name_, ui_name_, stick_gate, ReshapeKind::AnalogStick)
{
}

AnalogStick::AnalogStick(const char* const name_, const char* const ui_name_,
                         const StickGate& stick_gate, ReshapeKind kind)
    : ReshapableInput(name_, ui_name_, GroupType::Stick, kind), m_stick_gate(stick_gate)
{
  for (auto& named_direction : named_directions)
    AddInput(Translatability::Translate, named_direction);

  AddInput(Translatability::Translate, _trans("Modifier"));
}

AnalogStick::ReshapeData AnalogStick::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[0]->GetState() - controls[1]->GetState();
  const ControlState x = controls[3]->GetState() - controls[2]->GetState();

  // Return raw values. (used in UI)
  if (!adjusted)
    return {x, y};

  return Reshape(x, y, GetModifierInput()->GetState());
}

AnalogStick::StateData AnalogStick::GetState() const
{
  return GetReshapableState(true);
}

ControlState AnalogStick::GetGateRadiusAtAngle(double ang) const
{
  return m_stick_gate.GetRadiusAtAngle(ang);
}

Control* AnalogStick::GetModifierInput() const
{
  return controls[4];
}

OctagonAnalogStick::OctagonAnalogStick(const char* name_, ControlState gate_radius)
    : OctagonAnalogStick(name_, name_, gate_radius)
{
}

OctagonAnalogStick::OctagonAnalogStick(const char* name_, const char* ui_name_,
                                       ControlState gate_radius)
    : AnalogStick(name_, ui_name_, ControllerEmu::OctagonStickGate(1.0),
                  ReshapeKind::OctagonAnalogStick)
{
  AddVirtualNotchSetting(&m_virtual_notch_setting, 45);

  AddSetting(
      &m_gate_size_setting,
      {_trans("Gate Size"),
       // i18n: The percent symbol.
       _trans("%"),
       // i18n: Refers to plastic shell of game controller (stick gate) that limits stick movements.
       _trans("Adjusts target radius of simulated stick gate."), nullptr,
       SettingVisibility::Advanced},
      gate_radius * 100, 0.01, 100);
}

ControlState OctagonAnalogStick::GetVirtualNotchSize() const
{
  return m_virtual_notch_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState OctagonAnalogStick::GetGateRadiusAtAngle(double ang) const
{
  return AnalogStick::GetGateRadiusAtAngle(ang) * m_gate_size_setting.GetValue() / 100;
}

}  // namespace ControllerEmu
