// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControllerEmu.h"

#include <memory>
#include <string>
#include <utility>

#include "Common/IniFile.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ControllerEmu
{
// This should theoretically be per EmulatedController instance,
// though no EmulatedController usually run in parallel, so it makes little difference

std::string ControlGroupContainer::GetDisplayName() const
{
  return GetName();
}

EmulatedController::~EmulatedController()
{
  delete static_cast<DefaultDevice*>(retro_atomic_exchange_ptr(&m_default_device, nullptr));
}

EmulatedController::EmulatedController() = default;

void EmulatedController::FreeDefaultDevice(quiesce_node_t* node)
{
  delete reinterpret_cast<DefaultDevice*>(node);
}

void EmulatedController::UpdateReferences(const ControllerInterface& devi)
{
  retro_atomic_store_release_int(&m_rebind_requested, 1);
  // Pairs with the fence below: either this thread finds the role free, or the thread that
  // held it sees the request.
  retro_atomic_thread_fence_seq_cst();
  for (;;)
  {
    // Someone else is rebinding: they see the request before they stop.
    if (!retro_atomic_cas_int(&m_rebinding, 0, 1))
      return;
    while (retro_atomic_exchange_int(&m_rebind_requested, 0))
      RebindNow(devi);
    retro_atomic_store_release_int(&m_rebinding, 0);
    // A request that came in after the last check, while this thread still held the role.
    retro_atomic_thread_fence_seq_cst();
    if (!retro_atomic_load_seq_cst_int(&m_rebind_requested))
      return;
  }
}

void EmulatedController::RebindNow(const ControllerInterface& devi)
{
  const ciface::Core::DeviceQualifier default_device = GetDefaultDevice();
  retro_atomic_store_release_int(&m_default_device_is_connected,
                                 devi.HasConnectedDevice(default_device));

  ciface::ExpressionParser::ControlEnvironment env(devi, default_device, m_expression_vars);

  UpdateGroupsReferences(env);

  env.CleanUnusedVariables();
}

void ControlGroupContainer::UpdateGroupsReferences(
    ciface::ExpressionParser::ControlEnvironment& env)
{
  for (auto& group : groups)
    group->UpdateReferences(env);
}

void EmulatedController::UpdateSingleControlReference(const ControllerInterface& devi,
                                                      ControlReference*)
{
  // The environment's variables belong to the thread doing a rebind, so this is one.
  UpdateReferences(devi);
}

bool EmulatedController::IsDefaultDeviceConnected() const
{
  return retro_atomic_load_acquire_int(
             const_cast<retro_atomic_int_t*>(&m_default_device_is_connected)) != 0;
}

ciface::Core::DeviceQualifier EmulatedController::GetDefaultDevice() const
{
  ciface::InputReadScope scope;
  const auto* d = static_cast<const DefaultDevice*>(retro_atomic_load_acquire_ptr(&m_default_device));
  return d ? d->qualifier : ciface::Core::DeviceQualifier{};
}

void EmulatedController::SetDefaultDevice(const std::string& device)
{
  ciface::Core::DeviceQualifier devq;
  devq.FromString(device);
  SetDefaultDevice(std::move(devq));
}

void EmulatedController::SetDefaultDevice(ciface::Core::DeviceQualifier devq)
{
  auto* d = new DefaultDevice{{}, std::move(devq)};
  if (void* old = retro_atomic_exchange_ptr(&m_default_device, d))
    quiesce_retire(ciface::InputQuiesce(), &static_cast<DefaultDevice*>(old)->node,
                   FreeDefaultDevice);
}

ControlGroupContainer::~ControlGroupContainer() = default;

void EmulatedController::LoadConfig(Common::IniFile::Section* sec)
{
  std::string defdev;
  if (sec->Get("Device", &defdev, "") && !defdev.empty())
    SetDefaultDevice(defdev);

  LoadGroupsConfig(sec, "");
}

void ControlGroupContainer::LoadGroupsConfig(Common::IniFile::Section* sec, const std::string& base)
{
  for (auto& cg : groups)
    cg->LoadConfig(sec, base);
}

void EmulatedController::SaveConfig(Common::IniFile::Section* sec)
{
  sec->Set("Device", GetDefaultDevice().ToString(), "");

  SaveGroupsConfig(sec, "");
}

void ControlGroupContainer::SaveGroupsConfig(Common::IniFile::Section* sec, const std::string& base)
{
  for (auto& cg : groups)
    cg->SaveConfig(sec, base);
}

void EmulatedController::LoadDefaults(const ControllerInterface& ciface)
{
  // load an empty inifile section, clears everything
  Common::IniFile::Section sec;
  LoadConfig(&sec);

  const std::string& default_device_string = ciface.GetDefaultDeviceString();
  if (!default_device_string.empty())
  {
    SetDefaultDevice(default_device_string);
  }
}

}  // namespace ControllerEmu
