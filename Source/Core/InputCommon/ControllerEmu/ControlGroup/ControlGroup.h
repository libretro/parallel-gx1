// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Common/IniFile.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class Control;

class NumericSetting;
struct NumericSettingDetails;

template <typename T>
class SettingValue;

enum class GroupType
{
  Other,
  Stick,
  MixedTriggers,
  Buttons,
  Force,
  Attachments,
  Tilt,
  Cursor,
  Triggers,
  Slider,
  Shake,
  IMUAccelerometer,
  IMUGyroscope,
  IMUCursor,
  IRPassthrough,
};

class ControlGroup
{
public:
  enum class DefaultValue
  {
    AlwaysEnabled,
    Enabled,
    Disabled,
  };

  explicit ControlGroup(const char* name, GroupType type = GroupType::Other,
                        DefaultValue default_value = DefaultValue::AlwaysEnabled);
  ControlGroup(const char* name, const char* ui_name, GroupType type = GroupType::Other,
               DefaultValue default_value = DefaultValue::AlwaysEnabled);
  virtual ~ControlGroup();

  ControlGroup(const ControlGroup&) = delete;
  ControlGroup& operator=(const ControlGroup&) = delete;

  // These branch on the group's type to the reshapable inputs' and the attachments' own
  void LoadConfig(Common::IniFile::Section* sec, const char* base);
  void SaveConfig(Common::IniFile::Section* sec, const char* base);

  void UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env);

  void SetControlExpression(int index, const std::string& expression);

  // The names are literals the controls lend
  void AddInput(Translatability translate, const char* name);
  void AddInput(Translatability translate, const char* name, const char* ui_name);
  void AddOutput(Translatability translate, const char* name);

  template <typename T>
  void AddSetting(SettingValue<T>* value, const NumericSettingDetails& details,
                  std::common_type_t<T> default_value_, std::common_type_t<T> min_value = {},
                  std::common_type_t<T> max_value = T(100))
  {
    PushSetting(new NumericSetting(value, details, T(default_value_), T(min_value), T(max_value)));
  }

  void AddVirtualNotchSetting(SettingValue<double>* value, double max_virtual_notch_deg);

  void AddDeadzoneSetting(SettingValue<double>* value, double maximum_deadzone);

  template <typename T>
  static T ApplyDeadzone(T input, std::common_type_t<T> deadzone)
  {
    return std::copysign(std::max(T{0}, std::abs(input) - deadzone) / (T{1} - deadzone), input);
  }

  bool HasEnabledSetting() const;

  // Literals the group lends, as every group's names are
  const char* const name;
  const char* const ui_name;
  const GroupType type;

protected:
  // What every group does with its controls and settings
  void LoadConfigDefault(Common::IniFile::Section* sec, const char* base);
  void SaveConfigDefault(Common::IniFile::Section* sec, const char* base);

  // Room for a key: the group's prefix, a control's name and "/Range"
  static constexpr size_t INI_KEY_SIZE = 256;
  void UpdateReferencesDefault(ciface::ExpressionParser::ControlEnvironment& env);

  // Each takes the item, behind those the group has
  void PushControl(Control* control);
  void PushSetting(NumericSetting* setting);

public:
  // The default "enabled" state.
  const DefaultValue default_value;
  SettingValue<bool> enabled;
  // Owned, null for a group always enabled
  NumericSetting* enabled_setting = nullptr;

  // Owned, the first num_controls and num_numeric_settings of them: made with the group
  Control** controls = nullptr;
  int num_controls = 0;
  NumericSetting** numeric_settings = nullptr;
  int num_numeric_settings = 0;

private:
  int m_cap_controls = 0;
  int m_cap_numeric_settings = 0;
};
}  // namespace ControllerEmu
