// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputConfig.h"

#include <cstdio>
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

InputConfig::InputConfig(const char* ini_name, const char* gui_name,
                         const char* profile_directory_name, const char* profile_key)
    : m_ini_name(ini_name), m_gui_name(gui_name), m_profile_directory_name(profile_directory_name),
      m_profile_key(profile_key)
{
}

InputConfig::~InputConfig()
{
  ClearControllers();
}

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
      char profile_name[64];
      snprintf(profile_name, sizeof(profile_name), "%sProfile%s", m_profile_key, num[i]);

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
      inifile.NumSections() != 0)
  {
    int n;

    for (n = 0; n < m_num_controllers; n++)
    {
      ControllerEmu::EmulatedController* const controller = m_controllers[n];
      Common::IniFile::Section* config;
      // Load settings from ini
      if (useProfile[n])
      {
        std::string base;
        SplitPath(profile[n], nullptr, &base, nullptr);
        Core::DisplayMessage("Loading game specific input profile '%s' for device '%s'",
                             base.c_str(), controller->GetName());

        inifile.Load(profile[n]);
        config = inifile.GetOrCreateSection("Profile");
      }
      else
      {
        config = inifile.GetOrCreateSection(controller->GetName());
      }
      controller->LoadConfig(config);
      controller->UpdateReferences(g_controller_interface);
    }
    return true;
  }
  else
  {
    // Only load the default profile for the first controller and clear the others,
    // otherwise they would all share the same mappings on the same (default) device
    int i;
    if (m_num_controllers > 0)
    {
      m_controllers[0]->LoadDefaults(g_controller_interface);
      m_controllers[0]->UpdateReferences(g_controller_interface);
    }
    for (i = 1; i < m_num_controllers; ++i)
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
  int i;
  inifile.Load(ini_filename);

  for (i = 0; i < m_num_controllers; i++)
    m_controllers[i]->SaveConfig(inifile.GetOrCreateSection(m_controllers[i]->GetName()));

  inifile.Save(ini_filename);
}

ControllerEmu::EmulatedController* InputConfig::GetController(int index) const
{
  if (index < 0 || index >= m_num_controllers)
    return nullptr;
  return m_controllers[index];
}

void InputConfig::ClearControllers()
{
  int i;
  for (i = 0; i < m_num_controllers; i++)
    delete m_controllers[i];
  m_num_controllers = 0;
}

bool InputConfig::ControllersNeedToBeCreated() const
{
  return m_num_controllers == 0;
}

std::string InputConfig::GetUserProfileDirectoryPath() const
{
  return File::GetUserPath(D_CONFIG_IDX) + "Profiles/" + m_profile_directory_name + "/";
}

void InputConfig::RegisterHotplugCallback()
{
  // Update control references on all controllers
  // as configured devices may have been added or removed.
  m_hotplug_event_hook = g_controller_interface.RegisterDevicesChangedCallback(
      [](void* user) {
        InputConfig* const config = static_cast<InputConfig*>(user);
        int i;
        for (i = 0; i < config->m_num_controllers; i++)
          config->m_controllers[i]->UpdateReferences(g_controller_interface);
      },
      this);
}

void InputConfig::UnregisterHotplugCallback()
{
  m_hotplug_event_hook.reset();
}

void InputConfig::GenerateControllerTextures(const Common::IniFile& file)
{
  m_dynamic_input_tex_config_manager.Load();

  std::vector<std::string> controller_names;
  int i;
  for (i = 0; i < m_num_controllers; i++)
    controller_names.push_back(m_controllers[i]->GetName());

  m_dynamic_input_tex_config_manager.GenerateTextures(file, controller_names);
}

void InputConfig::GenerateControllerTextures()
{
  const std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  int i;
  inifile.Load(ini_filename);

  for (i = 0; i < m_num_controllers; i++)
    m_controllers[i]->SaveConfig(inifile.GetOrCreateSection(m_controllers[i]->GetName()));

  GenerateControllerTextures(inifile);
}
