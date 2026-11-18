// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

#include <cstdlib>

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
    enabled_setting = new NumericSetting(&enabled, NumericSettingDetails{_trans("Enabled")},
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

ControlGroup::~ControlGroup()
{
  int i;
  for (i = 0; i < num_numeric_settings; i++)
    delete numeric_settings[i];
  std::free(numeric_settings);
  for (i = 0; i < num_controls; i++)
    delete controls[i];
  std::free(controls);
  delete enabled_setting;
}

/* The arrays grow while the group is made; a controller cannot be made
 * without its inputs, so running out of memory here ends the program, as
 * the vectors these were did. */
void ControlGroup::PushControl(Control* control)
{
  if (num_controls == m_cap_controls)
  {
    const int cap = m_cap_controls ? m_cap_controls * 2 : 8;
    Control** const grown = static_cast<Control**>(std::realloc(controls, cap * sizeof(*grown)));
    if (!grown)
      std::abort();
    controls = grown;
    m_cap_controls = cap;
  }
  controls[num_controls++] = control;
}

void ControlGroup::PushSetting(NumericSetting* setting)
{
  if (num_numeric_settings == m_cap_numeric_settings)
  {
    const int cap = m_cap_numeric_settings ? m_cap_numeric_settings * 2 : 4;
    NumericSetting** const grown =
        static_cast<NumericSetting**>(std::realloc(numeric_settings, cap * sizeof(*grown)));
    if (!grown)
      std::abort();
    numeric_settings = grown;
    m_cap_numeric_settings = cap;
  }
  numeric_settings[num_numeric_settings++] = setting;
}

void ControlGroup::LoadConfigDefault(Common::IniFile::Section* sec, const char* base)
{
  char group[INI_KEY_SIZE];
  char key[INI_KEY_SIZE];
  Common::JoinIniKey(group, sizeof(group), base, name.c_str(), "/");

  // enabled
  if (HasEnabledSetting())
    enabled_setting->LoadFromIni(*sec, group);

  int i;
  for (i = 0; i < num_numeric_settings; i++)
    numeric_settings[i]->LoadFromIni(*sec, group);

  for (i = 0; i < num_controls; i++)
  {
    Control* const c = controls[i];
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

  int i;
  for (i = 0; i < num_numeric_settings; i++)
    numeric_settings[i]->SaveToIni(*sec, group);

  for (i = 0; i < num_controls; i++)
  {
    Control* const c = controls[i];
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

  int i;
  for (i = 0; i < num_controls; i++)
    controls[i]->control_ref->UpdateReference(env);

  for (i = 0; i < num_numeric_settings; i++)
    numeric_settings[i]->GetInputReference().UpdateReference(env);
}

void ControlGroup::SetControlExpression(int index, const std::string& expression)
{
  if (index >= 0 && index < num_controls)
    controls[index]->control_ref->SetExpression(expression);
}

void ControlGroup::AddInput(Translatability translate, std::string name_)
{
  PushControl(new Input(translate, std::move(name_)));
}

void ControlGroup::AddInput(Translatability translate, std::string name_, std::string ui_name_)
{
  PushControl(new Input(translate, std::move(name_), std::move(ui_name_)));
}

void ControlGroup::AddOutput(Translatability translate, std::string name_)
{
  PushControl(new Output(translate, std::move(name_)));
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
