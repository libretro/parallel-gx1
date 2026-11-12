// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputProfile.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

namespace InputProfile
{
namespace
{
constexpr int display_message_ms = 3000;
}

std::vector<std::string> GetProfilesFromSetting(const std::string& setting, const std::string& root)
{
  const auto& setting_choices = SplitString(setting, ',');

  std::vector<std::string> result;
  for (const std::string& setting_choice : setting_choices)
  {
    const std::string path = root + std::string(StripWhitespace(setting_choice));
    if (File::IsDirectory(path))
    {
      const auto files_under_directory = Common::DoFileSearch(path, ".ini", true);
      result.insert(result.end(), files_under_directory.begin(), files_under_directory.end());
    }
    else
    {
      const std::string file_path = path + ".ini";
      if (File::Exists(file_path))
      {
        result.push_back(file_path);
      }
    }
  }

  return result;
}

std::vector<std::string> ProfileCycler::GetProfilesForDevice(InputConfig* device_configuration)
{
  const std::string device_profile_root_location(
      device_configuration->GetUserProfileDirectoryPath());
  return Common::DoFileSearch(device_profile_root_location, ".ini", true);
}

std::string ProfileCycler::GetWiimoteInputProfilesForGame(int controller_index)
{
  Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
  const auto* const control_section = game_ini.GetOrCreateSection("Controls");

  std::string result;
  char key[32];
  snprintf(key, sizeof(key), "WiimoteProfile%d", controller_index + 1);
  control_section->Get(key, &result);
  return result;
}

}  // namespace InputProfile
