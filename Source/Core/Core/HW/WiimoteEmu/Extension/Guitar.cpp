// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteEmu/Extension/Guitar.h"

#include <map>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Slider.h"
#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

namespace WiimoteEmu
{
static const std::map<const ControlState, const u8> s_slider_bar_control_codes{
    // values determined using a PS3 Guitar Hero 5 controller, which maps the touchbar to Zr on
    // Windows
    {0.0, 0x0F},        // not touching
    {-0.4375, 0x04},    // top fret
    {-0.097656, 0x0A},  // second fret
    {0.203125, 0x12},   // third fret
    {0.578125, 0x17},   // fourth fret
    {1.0, 0x1F}         // bottom fret
};

static const u8 guitar_id[6] = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x03};

static const u16 guitar_fret_bitmasks[5] = {
    Guitar::FRET_GREEN, Guitar::FRET_RED,    Guitar::FRET_YELLOW,
    Guitar::FRET_BLUE,  Guitar::FRET_ORANGE,
};

static const char* const guitar_fret_names[5] = {
    _trans("Green"), _trans("Red"), _trans("Yellow"), _trans("Blue"), _trans("Orange"),
};

static const u16 guitar_button_bitmasks[2] = {
    Guitar::BUTTON_MINUS,
    Guitar::BUTTON_PLUS,
};

static const u16 guitar_strum_bitmasks[2] = {
    Guitar::BAR_UP,
    Guitar::BAR_DOWN,
};

Guitar::Guitar() : Extension1stParty(_trans("Guitar"))
{
  using Translatability = ControllerEmu::Translatability;

  // frets
  AddGroup(m_frets = new ControllerEmu::Buttons(_trans("Frets")));
  for (auto& guitar_fret_name : guitar_fret_names)
  {
    m_frets->AddInput(Translatability::Translate, guitar_fret_name);
  }

  // strum
  AddGroup(m_strum = new ControllerEmu::Buttons(_trans("Strum")));
  m_strum->AddInput(Translatability::Translate, _trans("Up"));
  m_strum->AddInput(Translatability::Translate, _trans("Down"));

  // buttons
  AddGroup(m_buttons = new ControllerEmu::Buttons(_trans("Buttons")));
  m_buttons->AddInput(Translatability::DoNotTranslate, "-");
  m_buttons->AddInput(Translatability::DoNotTranslate, "+");

  // stick
  constexpr auto gate_radius = ControlState(STICK_GATE_RADIUS) / STICK_RADIUS;
  AddGroup(m_stick = new ControllerEmu::OctagonAnalogStick(_trans("Stick"), gate_radius));

  // whammy
  AddGroup(m_whammy = new ControllerEmu::Triggers(_trans("Whammy")));
  m_whammy->AddInput(Translatability::Translate, _trans("Bar"));

  // slider bar
  AddGroup(m_slider_bar = new ControllerEmu::Slider(_trans("Slider Bar")));
}

void Guitar::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  using ControllerEmu::MapFloat;

  DataFormat guitar_data = {};

  // stick
  {
    const ControllerEmu::AnalogStick::StateData stick_state = m_stick->GetState();

    guitar_data.sx = MapFloat<u8>(stick_state.x, STICK_CENTER, 0, STICK_RANGE);
    guitar_data.sy = MapFloat<u8>(stick_state.y, STICK_CENTER, 0, STICK_RANGE);
  }

  // slider bar
  if (m_slider_bar->controls[0]->control_ref->BoundCount() &&
      m_slider_bar->controls[1]->control_ref->BoundCount())
  {
    const ControllerEmu::Slider::StateData slider_data = m_slider_bar->GetState();

    guitar_data.sb = s_slider_bar_control_codes.lower_bound(slider_data.value)->second;
  }
  else
  {
    // if user has not mapped controls for slider bar, tell game it's untouched
    guitar_data.sb = 0x0F;
  }

  // whammy bar
  const ControllerEmu::Triggers::StateData whammy_state = m_whammy->GetState();
  guitar_data.whammy = MapFloat<u8>(whammy_state.data[0], 0, 0, 0x1F);

  // buttons
  m_buttons->GetState(&guitar_data.bt, guitar_button_bitmasks);

  // frets
  m_frets->GetState(&guitar_data.bt, guitar_fret_bitmasks);

  // strum
  m_strum->GetState(&guitar_data.bt, guitar_strum_bitmasks);

  // flip button bits
  guitar_data.bt ^= 0xFFFF;

  target_state->type = ExtensionNumber::GUITAR;
  target_state->data.guitar = guitar_data;
}

void Guitar::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate(
      &m_reg, target_state.type == ExtensionNumber::GUITAR ? &target_state.data.guitar : nullptr,
      sizeof(DataFormat));
}

void Guitar::Reset()
{
  EncryptedExtension::Reset();

  memcpy(m_reg.identifier, guitar_id, sizeof(guitar_id));

  // TODO: Is there calibration data?
}

ControllerEmu::ControlGroup* Guitar::GetGroup(GuitarGroup group)
{
  switch (group)
  {
  case GuitarGroup::Buttons:
    return m_buttons;
  case GuitarGroup::Frets:
    return m_frets;
  case GuitarGroup::Strum:
    return m_strum;
  case GuitarGroup::Whammy:
    return m_whammy;
  case GuitarGroup::Stick:
    return m_stick;
  case GuitarGroup::SliderBar:
    return m_slider_bar;
  default:
    ASSERT(false);
    return nullptr;
  }
}
}  // namespace WiimoteEmu
