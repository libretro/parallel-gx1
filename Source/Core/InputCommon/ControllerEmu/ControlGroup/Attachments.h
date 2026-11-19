// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
class AttachedController : public ControlGroupContainer
{
};

// A container of the selected and available attachments
// for configuration saving/loading purposes
class Attachments : public ControlGroup
{
public:
  explicit Attachments(const std::string& name);
  ~Attachments() override;

  // Room for every Wii Remote extension
  enum
  {
    MAX_ATTACHMENTS = 16
  };

  // Takes the attachment, behind those the group has
  void AddAttachment(AttachedController* att);

  u32 GetSelectedAttachment() const;
  void SetSelectedAttachment(u32 val);

  NumericSetting& GetSelectionSetting();

  AttachedController* GetAttachment(u32 index) const { return m_attachments[index]; }
  u32 GetAttachmentCount() const { return m_num_attachments; }

  void LoadConfig(Common::IniFile::Section* sec, const char* base);
  void SaveConfig(Common::IniFile::Section* sec, const char* base);

  void UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env);

private:
  SettingValue<int> m_selection_value;
  // This is here and not added to the list of numeric_settings because it's serialized differently,
  // by string (to be independent from the enum), and visualized differently in the UI.
  // For the rest, it's treated similarly to other numeric_settings in the group.
  NumericSetting m_selection_setting = {&m_selection_value, {""}, 0, 0, 0};

  // Owned, the first m_num_attachments of them
  AttachedController* m_attachments[MAX_ATTACHMENTS];
  u32 m_num_attachments = 0;
};
}  // namespace ControllerEmu
