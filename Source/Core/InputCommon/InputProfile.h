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
}  // namespace InputProfile
