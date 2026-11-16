// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
NumericSetting::NumericSetting(SettingValue<int>* value, const NumericSettingDetails& details,
                               int default_value, int, int)
    : m_bits(&value->m_value), m_input(&value->m_input), m_ini_name(details.ini_name),
      m_default_value(default_value), m_type(Type::Int)
{
  SetToDefault();
}

NumericSetting::NumericSetting(SettingValue<double>* value, const NumericSettingDetails& details,
                               double default_value, double, double)
    : m_bits(&value->m_value), m_input(&value->m_input), m_ini_name(details.ini_name),
      m_default_value(default_value), m_type(Type::Double)
{
  SetToDefault();
}

NumericSetting::NumericSetting(SettingValue<bool>* value, const NumericSettingDetails& details,
                               bool default_value, bool, bool)
    : m_bits(&value->m_value), m_input(&value->m_input), m_ini_name(details.ini_name),
      m_default_value(default_value ? 1 : 0), m_type(Type::Bool)
{
  SetToDefault();
}

double NumericSetting::GetCachedValue() const
{
  const s64 bits = retro_atomic_load_relaxed_64(m_bits);
  switch (m_type)
  {
  case Type::Int:
  {
    int value;
    SettingFromBits(bits, &value);
    return value;
  }
  case Type::Bool:
  {
    bool value;
    SettingFromBits(bits, &value);
    return value ? 1 : 0;
  }
  default:
  {
    double value;
    SettingFromBits(bits, &value);
    return value;
  }
  }
}

void NumericSetting::SetValue(double value)
{
  s64 bits;
  switch (m_type)
  {
  case Type::Int:
    bits = SettingToBits(static_cast<int>(value));
    break;
  case Type::Bool:
    bits = SettingToBits(value != 0);
    break;
  default:
    bits = SettingToBits(value);
    break;
  }
  retro_atomic_store_relaxed_64(m_bits, bits);

  // Clear the expression to use our new "simple" value.
  m_input->SetExpression("");
}

void NumericSetting::LoadFromIni(const Common::IniFile::Section& section, const char* group_name)
{
  char key[256];
  std::string str_value;
  if (Common::JoinIniKey(key, sizeof(key), group_name, m_ini_name) && section.Get(key, &str_value))
  {
    m_input->SetExpression(std::move(str_value));
    SimplifyIfPossible();
  }
  else
  {
    SetValue(m_default_value);
  }
}

void NumericSetting::SaveToIni(Common::IniFile::Section& section, const char* group_name) const
{
  char key[256];
  if (!Common::JoinIniKey(key, sizeof(key), group_name, m_ini_name))
    return;
  if (IsSimpleValue())
  {
    const double value = GetCachedValue();
    switch (m_type)
    {
    case Type::Int:
      section.Set(key, static_cast<int>(value), static_cast<int>(m_default_value));
      break;
    case Type::Bool:
      section.Set(key, value != 0, m_default_value != 0);
      break;
    default:
      section.Set(key, value, m_default_value);
      break;
    }
  }
  else
  {
    // We can't save line breaks in a single line config. Restoring them is too complicated.
    std::string expression = m_input->GetExpression();
    ReplaceBreaksWithSpaces(expression);
    section.Set(key, expression, "");
  }
}

void NumericSetting::SimplifyIfPossible()
{
  const std::string text(StripWhitespace(m_input->GetExpression()));
  switch (m_type)
  {
  case Type::Int:
  {
    int value;
    if (TryParse(text, &value))
      SetValue(value);
    break;
  }
  case Type::Bool:
  {
    bool value;
    if (TryParse(text, &value))
      SetValue(value ? 1 : 0);
    break;
  }
  default:
  {
    double value;
    if (TryParse(text, &value))
      SetValue(value);
    break;
  }
  }
}
}  // namespace ControllerEmu
