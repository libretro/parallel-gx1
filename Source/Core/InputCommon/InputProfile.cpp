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

}  // namespace InputProfile
