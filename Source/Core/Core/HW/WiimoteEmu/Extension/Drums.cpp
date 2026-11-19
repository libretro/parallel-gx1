// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteEmu/Extension/Drums.h"

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace WiimoteEmu
{
static const u8 drums_id[6] = {0x01, 0x00, 0xa4, 0x20, 0x01, 0x03};

static const u8 drum_pad_bitmasks[6] = {
    Drums::PAD_RED,    Drums::PAD_YELLOW, Drums::PAD_BLUE,
    Drums::PAD_ORANGE, Drums::PAD_GREEN,  Drums::PAD_BASS,
};

static const char* const drum_pad_names[6] = {
    _trans("Red"),    _trans("Yellow"), _trans("Blue"),
    _trans("Orange"), _trans("Green"),  _trans("Bass"),
};

static const Drums::VelocityID drum_pad_velocity_ids[6] = {
    Drums::VelocityID::Red,    Drums::VelocityID::Yellow, Drums::VelocityID::Blue,
    Drums::VelocityID::Orange, Drums::VelocityID::Green,  Drums::VelocityID::Bass,
};

static const u8 drum_button_bitmasks[2] = {
    Drums::BUTTON_MINUS,
    Drums::BUTTON_PLUS,
};

Drums::Drums() : Extension1stParty("Drums", _trans("Drum Kit"))
{
  using Translatability = ControllerEmu::Translatability;

  // Pads.
  AddGroup(m_pads = new ControllerEmu::Buttons(_trans("Pads")));
  for (auto& drum_pad_name : drum_pad_names)
  {
    m_pads->AddInput(Translatability::Translate, drum_pad_name);
  }

  m_pads->AddSetting(&m_hit_strength_setting,
                     // i18n: Refers to how hard emulated drum pads are struck.
                     {_trans("Hit Strength"),
                      // i18n: The symbol for percent.
                      _trans("%")},
                     50);

  // Buttons.
  AddGroup(m_buttons = new ControllerEmu::Buttons(_trans("Buttons")));
  m_buttons->AddInput(Translatability::DoNotTranslate, "-");
  m_buttons->AddInput(Translatability::DoNotTranslate, "+");

  // Stick.
  AddGroup(m_stick = new ControllerEmu::OctagonAnalogStick(_trans("Stick"), GATE_RADIUS));
}

void Drums::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  using ControllerEmu::MapFloat;

  target_state->type = ExtensionNumber::DRUMS;
  DesiredState& state = target_state->data.drums;
  state = DesiredState{};

  {
    const ControllerEmu::AnalogStick::StateData stick_state = m_stick->GetState();

    state.stick_x = MapFloat<u8>(stick_state.x, STICK_CENTER, STICK_MIN, STICK_MAX);
    state.stick_y = MapFloat<u8>(stick_state.y, STICK_CENTER, STICK_MIN, STICK_MAX);
    state.stick_x = MapFloat(stick_state.x, STICK_CENTER, STICK_MIN, STICK_MAX);
    state.stick_y = MapFloat(stick_state.y, STICK_CENTER, STICK_MIN, STICK_MAX);
  }

  state.buttons = 0;
  m_buttons->GetState(&state.buttons, drum_button_bitmasks);

  state.drum_pads = 0;
  m_pads->GetState(&state.drum_pads, drum_pad_bitmasks);

  state.softness = u8(0x7F - std::lround(m_hit_strength_setting.GetValue() * 0x7F / 100));
}

void Drums::Update(const DesiredExtensionState& target_state)
{
  DesiredState desired_state;
  if (target_state.type == ExtensionNumber::DRUMS)
  {
    desired_state = target_state.data.drums;
  }
  else
  {
    // Set a sane default
    desired_state.stick_x = STICK_CENTER;
    desired_state.stick_y = STICK_CENTER;
    desired_state.buttons = 0;
    desired_state.drum_pads = 0;
    desired_state.softness = 0x7F;
  }

  DataFormat drum_data = {};
  u8 velocity = 0x7F;

  // The meaning of these bits are unknown but they are usually set.
  drum_data.unk1 = 0b11;
  drum_data.unk2 = 0b11;
  drum_data.unk5 = 0b11;

  // Send no velocity data by default.
  drum_data.velocity_id = u8(VelocityID::None);
  drum_data.no_velocity_data_1 = 1;
  drum_data.no_velocity_data_2 = 1;

  drum_data.stick_x = desired_state.stick_x;
  drum_data.stick_y = desired_state.stick_y;
  drum_data.buttons = desired_state.buttons;

  // Drum pads.
  u8 current_pad_input = desired_state.drum_pads;
  m_new_pad_hits |= ~m_prev_pad_input & current_pad_input;
  m_prev_pad_input = current_pad_input;

  static_assert(sizeof(m_pad_remaining_frames) ==
                    (sizeof(drum_pad_bitmasks) / sizeof(drum_pad_bitmasks[0])),
                "Array sizes do not match.");

  // Figure out which velocity id to send. (needs to be sent once for each newly hit drum-pad)
  for (std::size_t i = 0; i != (sizeof(drum_pad_bitmasks) / sizeof(drum_pad_bitmasks[0])); ++i)
  {
    const auto drum_pad = drum_pad_bitmasks[i];

    if (m_new_pad_hits & drum_pad)
    {
      // Clear the bit so velocity data is not sent again until the next hit.
      m_new_pad_hits &= ~drum_pad;

      drum_data.velocity_id = u8(drum_pad_velocity_ids[i]);

      drum_data.no_velocity_data_1 = 0;
      drum_data.no_velocity_data_2 = 0;

      velocity = desired_state.softness;

      // A drum-pad hit causes the relevant bit to be triggered for the next 10 frames.
      constexpr u8 HIT_FRAME_COUNT = 10;

      m_pad_remaining_frames[i] = HIT_FRAME_COUNT;

      break;
    }
  }
  drum_data.velocity0 = velocity;
  drum_data.velocity1 = velocity >> 1;
  drum_data.velocity2 = velocity >> 2;
  drum_data.velocity3 = velocity >> 3;
  drum_data.velocity4 = velocity >> 4;

  // Figure out which drum-pad bits to send.
  // Note: Relevant bits are not set until after velocity data has been sent.
  // My drums never exposed simultaneous hits. One pad bit was always sent before the other.
  for (std::size_t i = 0; i != (sizeof(drum_pad_bitmasks) / sizeof(drum_pad_bitmasks[0])); ++i)
  {
    auto& remaining_frames = m_pad_remaining_frames[i];

    if (remaining_frames != 0)
    {
      drum_data.drum_pads |= drum_pad_bitmasks[i];
      --remaining_frames;
    }
  }

  // Flip button and drum-pad bits. (0 == pressed)
  drum_data.buttons ^= 0x7e;
  drum_data.drum_pads ^= 0xff;

  // Copy data to proper region in the "register".
  Common::BitCastPtr<DataFormat>(&m_reg.controller_data) = drum_data;
}

void Drums::Reset()
{
  EncryptedExtension::Reset();

  memcpy(m_reg.identifier, drums_id, sizeof(drums_id));

  // Both 16-byte blocks of calibration data seem to be 0xff filled.
  memset(m_reg.calibration, 0xff, sizeof(m_reg.calibration));

  m_prev_pad_input = 0;
  m_new_pad_hits = 0;
  memset(m_pad_remaining_frames, 0, sizeof(m_pad_remaining_frames));
}

void Drums::DoState(PointerWrap& p)
{
  EncryptedExtension::DoState(p);

  p.Do(m_prev_pad_input);
  p.Do(m_new_pad_hits);
  p.Do(m_pad_remaining_frames);
}

ControllerEmu::ControlGroup* Drums::GetGroup(DrumsGroup group)
{
  switch (group)
  {
  case DrumsGroup::Buttons:
    return m_buttons;
  case DrumsGroup::Pads:
    return m_pads;
  case DrumsGroup::Stick:
    return m_stick;
  default:
    ASSERT(false);
    return nullptr;
  }
}
}  // namespace WiimoteEmu
