// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
NumericSettingBase::NumericSettingBase(const NumericSettingDetails& details) : m_details(details)
{
}

// Explicit instantiations so generic definitions can exist outside of the header.
template class NumericSetting<int>;
template class NumericSetting<double>;
template class NumericSetting<bool>;

template <typename T>
void NumericSetting<T>::SetExpressionFromValue()
{
  // Always include -/+ sign to prevent CoalesceExpression binding.
  // e.g. 1 is a valid input name for keyboard devices, +1 is not.
  char text[32];
  CharArrayFromFormat(text, "%+g", static_cast<double>(ControlState(GetValue())));
  m_value.m_input.SetExpression(text);
}

template <typename T>
void NumericSetting<T>::SimplifyIfPossible()
{
  ValueType value;
  if (TryParse(std::string(StripWhitespace(m_value.m_input.GetExpression())), &value))
    m_value.SetValue(value);
}

template <>
SettingType NumericSetting<int>::GetType() const
{
  return SettingType::Int;
}

template <>
SettingType NumericSetting<double>::GetType() const
{
  return SettingType::Double;
}

template <>
SettingType NumericSetting<bool>::GetType() const
{
  return SettingType::Bool;
}

}  // namespace ControllerEmu
