// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

#include "Common/IniFile.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Input.h"
#include "InputCommon/ControllerEmu/Control/Output.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace ControllerEmu
{
ControlGroup::ControlGroup(const std::string& name_, const GroupType type_,
                           DefaultValue default_value_)
    : ControlGroup{name_, name_, type_, default_value_}
{
}

ControlGroup::ControlGroup(std::string name_, std::string ui_name_, const GroupType type_,
                           DefaultValue default_value_)
    : name(std::move(name_)), ui_name(std::move(ui_name_)), type(type_),
      default_value(default_value_)
{
  if (default_value_ != DefaultValue::AlwaysEnabled)
  {
    enabled_setting =
        std::make_unique<NumericSetting>(&enabled, NumericSettingDetails{_trans("Enabled")},
                                         (default_value_ == DefaultValue::Enabled), false, true);
  }
}

void ControlGroup::AddVirtualNotchSetting(SettingValue<double>* value, double max_virtual_notch_deg)
{
  AddSetting(value,
             {_trans("Virtual Notches"),
              // i18n: The degrees symbol.
              _trans("°"), _trans("Snap the thumbstick position to the nearest octagonal axis."),
              nullptr, SettingVisibility::Advanced},
             0, 0, max_virtual_notch_deg);
}

void ControlGroup::AddDeadzoneSetting(SettingValue<double>* value, double maximum_deadzone)
{
  AddSetting(value,
             {_trans("Dead Zone"),
              // i18n: The percent symbol.
              _trans("%"),
              // i18n: Refers to the dead-zone setting of gamepad inputs.
              _trans("Input strength to ignore and remap.")},
             0, 0, maximum_deadzone);
}

ControlGroup::~ControlGroup() = default;

void ControlGroup::LoadConfigDefault(Common::IniFile::Section* sec, const char* base)
{
  char group[INI_KEY_SIZE];
  char key[INI_KEY_SIZE];
  Common::JoinIniKey(group, sizeof(group), base, name.c_str(), "/");

  // enabled
  if (HasEnabledSetting())
    enabled_setting->LoadFromIni(*sec, group);

  for (auto& setting : numeric_settings)
    setting->LoadFromIni(*sec, group);

  for (auto& c : controls)
  {
    {
      // control expression
      std::string expression;
      Common::JoinIniKey(key, sizeof(key), group, c->name.c_str());
      sec->Get(key, &expression, "");
      c->control_ref->SetExpression(std::move(expression));
    }

    // range
    double range = 100.0;
    Common::JoinIniKey(key, sizeof(key), group, c->name.c_str(), "/Range");
    sec->Get(key, &range, 100.0);
    c->control_ref->SetRange(range / 100);
  }
}

void ControlGroup::SaveConfigDefault(Common::IniFile::Section* sec, const char* base)
{
  char group[INI_KEY_SIZE];
  char key[INI_KEY_SIZE];
  Common::JoinIniKey(group, sizeof(group), base, name.c_str(), "/");

  // enabled
  if (HasEnabledSetting())
    enabled_setting->SaveToIni(*sec, group);

  for (auto& setting : numeric_settings)
    setting->SaveToIni(*sec, group);

  for (auto& c : controls)
  {
    // control expression
    std::string expression = c->control_ref->GetExpression();
    // We can't save line breaks in a single line config. Restoring them is too complicated.
    ReplaceBreaksWithSpaces(expression);
    if (Common::JoinIniKey(key, sizeof(key), group, c->name.c_str()))
      sec->Set(key, expression, "");

    // range
    if (Common::JoinIniKey(key, sizeof(key), group, c->name.c_str(), "/Range"))
      sec->Set(key, c->control_ref->GetRange() * 100.0, 100.0);
  }
}

void ControlGroup::UpdateReferencesDefault(ciface::ExpressionParser::ControlEnvironment& env)
{
  if (HasEnabledSetting())
    enabled_setting->GetInputReference().UpdateReference(env);

  for (auto& control : controls)
    control->control_ref->UpdateReference(env);

  for (auto& setting : numeric_settings)
    setting->GetInputReference().UpdateReference(env);
}

void ControlGroup::SetControlExpression(int index, const std::string& expression)
{
  controls.at(index)->control_ref->SetExpression(expression);
}

void ControlGroup::AddInput(Translatability translate, std::string name_)
{
  controls.emplace_back(std::make_unique<Input>(translate, std::move(name_)));
}

void ControlGroup::AddInput(Translatability translate, std::string name_, std::string ui_name_)
{
  controls.emplace_back(std::make_unique<Input>(translate, std::move(name_), std::move(ui_name_)));
}

void ControlGroup::AddOutput(Translatability translate, std::string name_)
{
  controls.emplace_back(std::make_unique<Output>(translate, std::move(name_)));
}

bool ControlGroup::HasEnabledSetting() const
{
  return enabled_setting != nullptr;
}

static bool IsReshapable(GroupType type)
{
  return type == GroupType::Stick || type == GroupType::Force || type == GroupType::Tilt ||
         type == GroupType::Cursor;
}

void ControlGroup::LoadConfig(Common::IniFile::Section* sec, const char* base)
{
  if (IsReshapable(type))
    static_cast<ReshapableInput*>(this)->LoadConfig(sec, base);
  else if (type == GroupType::Attachments)
    static_cast<Attachments*>(this)->LoadConfig(sec, base);
  else
    LoadConfigDefault(sec, base);
}

void ControlGroup::SaveConfig(Common::IniFile::Section* sec, const char* base)
{
  if (IsReshapable(type))
    static_cast<ReshapableInput*>(this)->SaveConfig(sec, base);
  else if (type == GroupType::Attachments)
    static_cast<Attachments*>(this)->SaveConfig(sec, base);
  else
    SaveConfigDefault(sec, base);
}

void ControlGroup::UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env)
{
  if (type == GroupType::Attachments)
    static_cast<Attachments*>(this)->UpdateReferences(env);
  else
    UpdateReferencesDefault(env);
}
}  // namespace ControllerEmu
