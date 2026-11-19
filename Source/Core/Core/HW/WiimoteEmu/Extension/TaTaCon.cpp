// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace WiimoteEmu
{
static const u8 tatacon_id[6] = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x11};

static const u8 center_bitmasks[2] = {
    TaTaCon::CENTER_LEFT,
    TaTaCon::CENTER_RIGHT,
};

static const u8 rim_bitmasks[2] = {
    TaTaCon::RIM_LEFT,
    TaTaCon::RIM_RIGHT,
};

static const char* const position_names[2] = {
    _trans("Left"),
    _trans("Right"),
};

// i18n: The drum controller used in "Taiko no Tatsujin" games. Also known as a "TaTaCon".
TaTaCon::TaTaCon() : Extension3rdParty("TaTaCon", _trans("Taiko Drum"))
{
  using Translatability = ControllerEmu::Translatability;

  // i18n: Refers to the "center" of a TaTaCon drum.
  AddGroup(m_center = new ControllerEmu::Buttons(_trans("Center")));
  for (auto& name : position_names)
    m_center->AddInput(Translatability::Translate, name);

  // i18n: Refers to the "rim" of a TaTaCon drum.
  AddGroup(m_rim = new ControllerEmu::Buttons(_trans("Rim")));
  for (auto& name : position_names)
    m_rim->AddInput(Translatability::Translate, name);
}

void TaTaCon::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DesiredState tatacon_data = {};

  m_center->GetState(&tatacon_data.state, center_bitmasks);
  m_rim->GetState(&tatacon_data.state, rim_bitmasks);

  target_state->type = ExtensionNumber::TATACON;
  target_state->data.tatacon = tatacon_data;
}

void TaTaCon::Update(const DesiredExtensionState& target_state)
{
  DesiredState desired_state{};
  if (target_state.type == ExtensionNumber::TATACON)
    desired_state = target_state.data.tatacon;

  // Flip button bits.
  desired_state.state ^= 0xff;

  m_reg.controller_data[5] = desired_state.state;
}

void TaTaCon::Reset()
{
  EncryptedExtension::Reset();

  m_reg = {};
  memcpy(m_reg.identifier, tatacon_id, sizeof(tatacon_id));

  // Assuming calibration is 0xff filled like every other 3rd-party extension.
  memset(m_reg.calibration, 0xff, sizeof(m_reg.calibration));
}

ControllerEmu::ControlGroup* TaTaCon::GetGroup(TaTaConGroup group)
{
  switch (group)
  {
  case TaTaConGroup::Center:
    return m_center;
  case TaTaConGroup::Rim:
    return m_rim;
  default:
    ASSERT(false);
    return nullptr;
  }
}
}  // namespace WiimoteEmu
