// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IniFile.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <retro_atomic.h>

namespace ControllerEmu
{
/* A setting's value lives in one atomic 64-bit word, as its bits. */
inline s64 SettingToBits(double value)
{
  s64 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
inline s64 SettingToBits(int value)
{
  return value;
}
inline s64 SettingToBits(bool value)
{
  return value ? 1 : 0;
}
inline void SettingFromBits(s64 bits, double* value)
{
  std::memcpy(value, &bits, sizeof(*value));
}
inline void SettingFromBits(s64 bits, int* value)
{
  *value = static_cast<int>(bits);
}
inline void SettingFromBits(s64 bits, bool* value)
{
  *value = bits != 0;
}

enum class SettingVisibility
{
  Normal,
  Advanced,
};

struct NumericSettingDetails
{
  NumericSettingDetails(const char* const _ini_name, const char* const _ui_suffix = nullptr,
                        const char* const _ui_description = nullptr,
                        const char* const _ui_name = nullptr,
                        SettingVisibility _visibility = SettingVisibility::Normal)
      : ini_name(_ini_name), ui_suffix(_ui_suffix), ui_description(_ui_description),
        ui_name(_ui_name ? _ui_name : _ini_name), visibility(_visibility)
  {
  }

  // The name used in ini files.
  const char* const ini_name;

  // A string applied to the number in the UI (unit of measure).
  const char* const ui_suffix;

  // Detailed description of the setting.
  const char* const ui_description;

  // The name used in the UI (if different from ini file).
  const char* const ui_name;

  // Advanced settings should be harder to change in the UI. They might confuse users.
  const SettingVisibility visibility;
};

template <typename T>
class SettingValue;

// A setting of a control group: the name its value is saved under, its default, and the value it
// sets, an int, a double or a bool, which a control expression may give instead.
class NumericSetting final
{
public:
  NumericSetting(SettingValue<int>* value, const NumericSettingDetails& details, int default_value,
                 int min_value, int max_value);
  NumericSetting(SettingValue<double>* value, const NumericSettingDetails& details,
                 double default_value, double min_value, double max_value);
  NumericSetting(SettingValue<bool>* value, const NumericSettingDetails& details,
                 bool default_value, bool min_value, bool max_value);

  void SetToDefault() { SetValue(m_default_value); }

  void LoadFromIni(const Common::IniFile::Section& section, const char* group_name);
  void SaveToIni(Common::IniFile::Section& section, const char* group_name) const;

  InputReference& GetInputReference() { return *m_input; }
  const InputReference& GetInputReference() const { return *m_input; }

  bool IsSimpleValue() const { return !m_input->HasExpression(); }

  // Convert a literal expression e.g. "7.0" to a regular value. (disables expression parsing)
  void SimplifyIfPossible();

  // The value, kept as the setting's type keeps it: an int truncated, a bool as not zero. The
  // expression is cleared, to use the new "simple" value.
  void SetValue(double value);

private:
  enum class Type
  {
    Int,
    Double,
    Bool,
  };

  // The value as its type keeps it, as a double
  double GetCachedValue() const;

  retro_atomic_64_t* m_bits;
  // Unfortunately InputReference's state grabbing is non-const requiring a pointer here.
  InputReference* m_input;
  const char* m_ini_name;
  double m_default_value;
  Type m_type;
};

template <typename T>
class SettingValue
{
  using ValueType = T;

  friend class NumericSetting;

public:
  ValueType GetValue() const
  {
    // Only update dynamic values when the input gate is enabled.
    // Otherwise settings will all change to 0 when window focus is lost.
    // This is very undesirable for things like battery level or attached extension.
    if (!IsSimpleValue() && ControlReference::GetInputGate())
      StoreValue(m_input.GetState<ValueType>());

    return GetCachedValue();
  }

  ValueType GetCachedValue() const
  {
    ValueType value;
    SettingFromBits(retro_atomic_load_relaxed_64(&m_value), &value);
    return value;
  }

  bool IsSimpleValue() const { return !m_input.HasExpression(); }

  void SetValue(const ValueType value)
  {
    StoreValue(value);

    // Clear the expression to use our new "simple" value.
    m_input.SetExpression("");
  }

private:
  void StoreValue(ValueType value) const
  {
    retro_atomic_store_relaxed_64(&m_value, SettingToBits(value));
  }

  // Values are R/W by both UI and CPU threads.
  mutable retro_atomic_64_t m_value{};

  // Unfortunately InputReference's state grabbing is non-const requiring mutable here.
  mutable InputReference m_input;
};

}  // namespace ControllerEmu
