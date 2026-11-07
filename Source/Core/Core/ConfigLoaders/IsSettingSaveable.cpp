// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ConfigLoaders/IsSettingSaveable.h"

#include <algorithm>
#include <array>

#include "Core/Config/WiimoteSettings.h"

namespace ConfigLoaders
{
bool IsSettingSaveable(const Config::Location& config_location)
{
  static constexpr std::array systems_not_saveable = {Config::System::GCPad, Config::System::WiiPad,
                                                      Config::System::GCKeyboard};

  bool not_saveable = false;
  for (const Config::System system : systems_not_saveable)
    not_saveable = not_saveable || system == config_location.system;
  if (!not_saveable)
  {
    return true;
  }

  static const auto s_setting_saveable = {
      &Config::WIIMOTE_1_SOURCE.GetLocation(),  &Config::WIIMOTE_2_SOURCE.GetLocation(),
      &Config::WIIMOTE_3_SOURCE.GetLocation(),  &Config::WIIMOTE_4_SOURCE.GetLocation(),
      &Config::WIIMOTE_BB_SOURCE.GetLocation(),
  };

  return std::ranges::any_of(s_setting_saveable, [&config_location](const auto* location) {
    return *location == config_location;
  });
}
}  // namespace ConfigLoaders
