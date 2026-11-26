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
      path_list_t files_under_directory;
      path_list_init(&files_under_directory);
      Common::DoFileSearch(path.c_str(), ".ini", true, &files_under_directory);
      for (unsigned i = 0; i < files_under_directory.count; i++)
        result.push_back(path_list_get(&files_under_directory, i));
      path_list_free(&files_under_directory);
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
