// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/DynamicInputTextureManager.h"

namespace Common
{
class IniFile;
}

namespace ControllerEmu
{
class EmulatedController;
}

class InputConfig
{
public:
  // The names are kept, not copied: literals.
  InputConfig(const char* ini_name, const char* gui_name, const char* profile_directory_name,
              const char* profile_key);

  ~InputConfig();

  bool LoadConfig();
  void SaveConfig();

  // A config holds a Wii Remote for each slot and the balance board at most
  enum
  {
    MAX_CONTROLLERS = 5
  };

  template <typename T, typename... Args>
  void CreateController(Args&&... args)
  {
    if (m_num_controllers < MAX_CONTROLLERS)
      m_controllers[m_num_controllers++] = new T(std::forward<Args>(args)...);
  }

  ControllerEmu::EmulatedController* GetController(int index) const;
  void ClearControllers();
  bool ControllersNeedToBeCreated() const;

  const char* GetGUIName() const { return m_gui_name; }
  const char* GetProfileKey() const { return m_profile_key; }
  const char* GetProfileDirectoryName() const { return m_profile_directory_name; }
  std::string GetUserProfileDirectoryPath() const;

  // These should be used after creating all controllers and before clearing them, respectively.
  void RegisterHotplugCallback();
  void UnregisterHotplugCallback();

  void GenerateControllerTextures(const Common::IniFile& file);
  void GenerateControllerTextures();

private:
  Common::EventHook m_hotplug_event_hook;
  // Owned, the first m_num_controllers of them
  ControllerEmu::EmulatedController* m_controllers[MAX_CONTROLLERS];
  int m_num_controllers = 0;
  const char* const m_ini_name;
  const char* const m_gui_name;
  const char* const m_profile_directory_name;
  const char* const m_profile_key;
  InputCommon::DynamicInputTextureManager m_dynamic_input_tex_config_manager;
};
