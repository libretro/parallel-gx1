// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>

#include "Common/HookableEvent.h"
#include "Common/Matrix.h"
#include "Common/WindowSystemInfo.h"

#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

#include <retro_atomic.h>

// enable disable sources
#define CIFACE_USE_DUALSHOCKUDPCLIENT
#if defined(HAVE_HIDAPI)
#define CIFACE_USE_STEAMDECK
#endif

namespace ciface
{
// A thread local "input channel" is maintained to handle the state of relative inputs.
// This allows simultaneous use of relative inputs across different input contexts.
// e.g. binding relative mouse movements to both GameCube controllers and Wii Remotes.
// These operate at different rates and processing one would break the other without separate state.
enum class InputChannel
{
  Host,
  SerialInterface,
  Bluetooth,
  Count,
};

}  // namespace ciface

//
// ControllerInterface
//
// Some crazy shit I made to control different device inputs and outputs
// from lots of different sources, hopefully more easily.
//
class ControllerInterface : public ciface::Core::DeviceContainer
{
public:
  enum class WindowChangeReason
  {
    // Application is shutting down
    Exit,
    Other
  };

  enum class RefreshReason
  {
    // Only the window changed.
    WindowChangeOnly,
    // User requested, or any other internal reason (e.g. init).
    // The window might have changed anyway.
    Other
  };

  ControllerInterface() {}
  void Initialize(const WindowSystemInfo& wsi);
  // Only call from one thread at a time.
  void ChangeWindow(void* hwnd, WindowChangeReason reason = WindowChangeReason::Other);
  // Can be called by any thread at any time (when initialized).
  void RefreshDevices(RefreshReason reason = RefreshReason::Other);
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

  // Set adjustment from the full render window aspect-ratio to the drawn aspect-ratio.
  // Used to fit mouse cursor inputs to the relevant region of the render window.
  void SetAspectRatioAdjustment(float);

  // Calculated from the aspect-ratio adjustment.
  // Inputs based on window coordinates should be multiplied by this.
  Common::Vec2 GetWindowInputScale() const;

  // Request that the mouse cursor should be centered in the render window at the next opportunity.
  void SetMouseCenteringRequested(bool center);

  bool IsMouseCenteringRequested() const;

  [[nodiscard]] Common::EventHook
  RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback);

  static void SetCurrentInputChannel(ciface::InputChannel);
  static ciface::InputChannel GetCurrentInputChannel();

  WindowSystemInfo GetWindowSystemInfo() const;

private:
  void ClearDevices();

  void InvokeDevicesChangedCallbacks();

  Common::HookableEvent<> m_devices_changed_event;

  retro_atomic_int_t m_is_init{};
  // Populations in progress on any thread; the last to finish runs the callbacks.
  retro_atomic_int_t m_populating_devices_counter{};
  WindowSystemInfo m_wsi;
  retro_atomic_int_t m_aspect_ratio_adjustment{0x3f800000}; /* the bits of a float, 1.0f */
  retro_atomic_int_t m_requested_mouse_centering{};

  std::vector<std::unique_ptr<ciface::InputBackend>> m_input_backends;
};

namespace ciface
{
template <typename T>
class RelativeInputState
{
public:
  void Update()
  {
    const auto channel = int(ControllerInterface::GetCurrentInputChannel());

    m_value[channel] = m_delta[channel];
    m_delta[channel] = {};
  }

  T GetValue() const
  {
    const auto channel = int(ControllerInterface::GetCurrentInputChannel());

    return m_value[channel];
  }

  void Move(T delta)
  {
    for (auto& d : m_delta)
      d += delta;
  }

private:
  std::array<T, int(InputChannel::Count)> m_value;
  std::array<T, int(InputChannel::Count)> m_delta;
};
}  // namespace ciface

extern ControllerInterface g_controller_interface;
