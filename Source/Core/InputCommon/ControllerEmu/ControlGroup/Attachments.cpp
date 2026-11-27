// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"

namespace ControllerEmu
{

Attachments::Attachments(const char* name_) : ControlGroup(name_, GroupType::Attachments)
{
}

Attachments::~Attachments()
{
  u32 i;
  for (i = 0; i < m_num_attachments; i++)
    delete m_attachments[i];
}

void Attachments::AddAttachment(AttachedController* att)
{
  if (m_num_attachments < MAX_ATTACHMENTS)
    m_attachments[m_num_attachments++] = att;
  else
    delete att;
}

u32 Attachments::GetSelectedAttachment() const
{
  // This is originally an int, treat it as such
  const int value = m_selection_value.GetValue();

  if (value > 0 && static_cast<u32>(value) < m_num_attachments)
    return u32(value);

  return 0;
}

void Attachments::SetSelectedAttachment(u32 val)
{
  m_selection_setting.SetValue(val);
}

NumericSetting& Attachments::GetSelectionSetting()
{
  return m_selection_setting;
}

void Attachments::LoadConfig(Common::IniFile::Section* sec, const char* base)
{
  char key[INI_KEY_SIZE];
  LoadConfigDefault(sec, base);

  SetSelectedAttachment(0);

  std::string attachment_text;
  Common::JoinIniKey(key, sizeof(key), base, name);
  sec->Get(key, &attachment_text, "");

  // First assume attachment string is a valid expression.
  // If it instead matches one of the names of our attachments it is overridden below.
  GetSelectionSetting().GetInputReference().SetExpression(attachment_text);

  u32 n;
  for (n = 0; n < m_num_attachments; n++)
  {
    AttachedController* const ai = m_attachments[n];
    const std::string attachment_name = ai->GetName();
    Common::JoinIniKey(key, sizeof(key), base, attachment_name.c_str(), "/");
    ai->LoadGroupsConfig(sec, key);

    if (attachment_name == attachment_text)
      SetSelectedAttachment(n);
  }
}

void Attachments::SaveConfig(Common::IniFile::Section* sec, const char* base)
{
  char key[INI_KEY_SIZE];
  SaveConfigDefault(sec, base);

  if (!Common::JoinIniKey(key, sizeof(key), base, name))
  {
  }
  else if (GetSelectionSetting().IsSimpleValue())
  {
    sec->Set(key, m_attachments[GetSelectedAttachment()]->GetName(), "None");
  }
  else
  {
    std::string expression = GetSelectionSetting().GetInputReference().GetExpression();
    ReplaceBreaksWithSpaces(expression);
    sec->Set(key, expression, "None");
  }

  u32 i;
  for (i = 0; i < m_num_attachments; i++)
  {
    if (Common::JoinIniKey(key, sizeof(key), base, m_attachments[i]->GetName().c_str(), "/"))
      m_attachments[i]->SaveGroupsConfig(sec, key);
  }
}

void Attachments::UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env)
{
  UpdateReferencesDefault(env);

  GetSelectionSetting().GetInputReference().UpdateReference(env);

  u32 i;
  for (i = 0; i < m_num_attachments; i++)
    m_attachments[i]->UpdateGroupsReferences(env);
}

}  // namespace ControllerEmu
