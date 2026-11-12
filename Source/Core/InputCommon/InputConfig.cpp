// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputConfig.h"

#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputProfile.h"

InputConfig::InputConfig(std::string ini_name, std::string gui_name,
                         std::string profile_directory_name, std::string profile_key)
    : m_ini_name(std::move(ini_name)), m_gui_name(std::move(gui_name)),
      m_profile_directory_name(std::move(profile_directory_name)),
      m_profile_key(std::move(profile_key))
{
}

InputConfig::~InputConfig() = default;

bool InputConfig::LoadConfig()
{
  Common::IniFile inifile;
  bool useProfile[MAX_BBMOTES] = {false, false, false, false, false};
  static const char* const num[MAX_BBMOTES] = {"1", "2", "3", "4", "BB"};
  std::string profile[MAX_BBMOTES];

  if (SConfig::GetInstance().GetGameID() != "00000000")
  {
    const std::string profile_directory = GetUserProfileDirectoryPath();

    Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
    auto* control_section = game_ini.GetOrCreateSection("Controls");

    for (int i = 0; i < 4; i++)
    {
      const std::string profile_name = GetProfileKey() + "Profile" + num[i];

      if (control_section->Exists(profile_name))
      {
        std::string profile_setting;
        if (control_section->Get(profile_name, &profile_setting))
        {
          auto profiles = InputProfile::GetProfilesFromSetting(profile_setting, profile_directory);

          if (profiles.empty())
          {
            // TODO: PanicAlert shouldn't be used for this.
            PanicAlert("No profiles found for game setting '%s'", profile_setting.c_str());
            continue;
          }

          // Use the first profile by default
          profile[i] = profiles[0];
          useProfile[i] = true;
        }
      }
    }
  }

  if (inifile.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini") &&
      !inifile.GetSections().empty())
  {
    int n = 0;

    for (auto& controller : m_controllers)
    {
      Common::IniFile::Section config;
      // Load settings from ini
      if (useProfile[n])
      {
        std::string base;
        SplitPath(profile[n], nullptr, &base, nullptr);
        Core::DisplayMessage("Loading game specific input profile '" + base + "' for device '" +
                                 controller->GetName() + "'",
                             6000);

        inifile.Load(profile[n]);
        config = *inifile.GetOrCreateSection("Profile");
      }
      else
      {
        config = *inifile.GetOrCreateSection(controller->GetName());
      }
      controller->LoadConfig(&config);
      controller->UpdateReferences(g_controller_interface);

      // Next profile
      n++;
    }
    return true;
  }
  else
  {
    // Only load the default profile for the first controller and clear the others,
    // otherwise they would all share the same mappings on the same (default) device
    if (m_controllers.size() > 0)
    {
      m_controllers[0]->LoadDefaults(g_controller_interface);
      m_controllers[0]->UpdateReferences(g_controller_interface);
    }
    for (size_t i = 1; i < m_controllers.size(); ++i)
    {
      // Calling the base version just clears all settings without overwriting them with a default
      m_controllers[i]->EmulatedController::LoadDefaults(g_controller_interface);
      m_controllers[i]->UpdateReferences(g_controller_interface);
    }
    return false;
  }
}

void InputConfig::SaveConfig()
{
  std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
  {
    controller->SaveConfig(inifile.GetOrCreateSection(controller->GetName()));
  }

  inifile.Save(ini_filename);
}

ControllerEmu::EmulatedController* InputConfig::GetController(int index) const
{
  if (index < 0 || index >= static_cast<int>(m_controllers.size()))
    return nullptr;
  return m_controllers.at(index).get();
}

void InputConfig::ClearControllers()
{
  m_controllers.clear();
}

bool InputConfig::ControllersNeedToBeCreated() const
{
  return m_controllers.empty();
}

std::string InputConfig::GetUserProfileDirectoryPath() const
{
  return File::GetUserPath(D_CONFIG_IDX) + "Profiles/" + GetProfileDirectoryName() + "/";
}

std::string InputConfig::GetSysProfileDirectoryPath() const
{
  return File::GetSysDirectory() + "Profiles/" + GetProfileDirectoryName() + "/";
}

void InputConfig::RegisterHotplugCallback()
{
  // Update control references on all controllers
  // as configured devices may have been added or removed.
  m_hotplug_event_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    for (auto& controller : m_controllers)
      controller->UpdateReferences(g_controller_interface);
  });
}

void InputConfig::UnregisterHotplugCallback()
{
  m_hotplug_event_hook.reset();
}

void InputConfig::GenerateControllerTextures(const Common::IniFile& file)
{
  m_dynamic_input_tex_config_manager.Load();

  std::vector<std::string> controller_names;
  for (auto& controller : m_controllers)
  {
    controller_names.push_back(controller->GetName());
  }

  m_dynamic_input_tex_config_manager.GenerateTextures(file, controller_names);
}

void InputConfig::GenerateControllerTextures()
{
  const std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
  {
    controller->SaveConfig(inifile.GetOrCreateSection(controller->GetName()));
  }

  GenerateControllerTextures(inifile);
}
