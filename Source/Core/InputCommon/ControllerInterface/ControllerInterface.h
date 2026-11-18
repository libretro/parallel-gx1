// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>

#include "Common/HookableEvent.h"
#include "Common/Matrix.h"

#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <retro_atomic.h>

//
// ControllerInterface
//
// Some crazy shit I made to control different device inputs and outputs
// from lots of different sources, hopefully more easily.
//
class ControllerInterface : public ciface::Core::DeviceContainer
{
public:
  ControllerInterface() {}
  void Initialize();
  // Can be called by any thread at any time (when initialized).
  void RefreshDevices();
  void Shutdown();
  bool AddDevice(std::shared_ptr<ciface::Core::Device> device);
  // Removes all the devices the function returns true to.
  // If all the devices shared ptrs need to be destroyed immediately,
  // set force_devices_release to true.
  void RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback,
                    bool force_devices_release = false);
  // Runs a backend's population, holding back the devices-changed callbacks until it is
  // done so they run once for the batch.
  void PlatformPopulateDevices(const std::function<void()>& callback);
  bool IsInit() const { return retro_atomic_load_acquire_int(&m_is_init) != 0; }
  void UpdateInput();

  [[nodiscard]] Common::EventHook
  RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback);

private:
  void ClearDevices();

  void InvokeDevicesChangedCallbacks();

  Common::HookableEvent<> m_devices_changed_event;

  retro_atomic_int_t m_is_init{};
  // Populations in progress on any thread; the last to finish runs the callbacks.
  retro_atomic_int_t m_populating_devices_counter{};
};

extern ControllerInterface g_controller_interface;
