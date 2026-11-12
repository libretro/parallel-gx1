// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class InputConfig;

namespace ControllerEmu
{
class EmulatedController;
}

#include <string>
#include <vector>

namespace InputProfile
{
std::vector<std::string> GetProfilesFromSetting(const std::string& setting,
                                                const std::string& root);

enum class CycleDirection : int
{
  Forward = 1,
  Backward = -1
};

class ProfileCycler
{
public:
private:
  std::vector<std::string> GetProfilesForDevice(InputConfig* device_configuration);
  std::string GetWiimoteInputProfilesForGame(int controller_index);

  int m_wiimote_profile_index = 0;
};
}  // namespace InputProfile
