// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"

#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#endif
#ifdef CIFACE_USE_STEAMDECK
#include "InputCommon/ControllerInterface/SteamDeck/SteamDeck.h"
#endif

ControllerInterface g_controller_interface;

// We need to save which input channel we are in by thread, so we can access the correct input
// update values in different threads by input channel. We start from InputChannel::Host on all
// threads as hotkeys are updated from a worker thread, but UI can read from the main thread. This
// will never interfere with game threads.
static thread_local ciface::InputChannel tls_input_channel = ciface::InputChannel::Host;

static thread_local bool tls_is_updating_devices = false;

void ControllerInterface::Initialize(const WindowSystemInfo& wsi)
{
  if (IsInit())
    return;

  m_wsi = wsi;

  retro_atomic_store_release_int(&m_populating_devices_counter, 1);

#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  m_input_backends.emplace_back(ciface::DualShockUDPClient::CreateInputBackend(this));
#endif
#ifdef CIFACE_USE_STEAMDECK
  m_input_backends.emplace_back(ciface::SteamDeck::CreateInputBackend(this));
#endif

  // Don't allow backends to add devices before the first RefreshDevices() as they will be cleaned
  // there. Or they'd end up waiting on the devices mutex if populated from another thread.
  retro_atomic_store_release_int(&m_is_init, 1);

  RefreshDevices();

  bool devices_empty;
  {
    ciface::InputReadScope scope;
    const DeviceList* const list = PeekDevices();
    devices_empty = !list || list->devices.empty();
  }

  if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1 && !devices_empty)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::ChangeWindow(void* hwnd, WindowChangeReason reason)
{
  if (!IsInit())
    return;

  if (m_wsi.render_window == hwnd)
    return;

  // This shouldn't use render_surface so no need to update it.
  m_wsi.render_window = hwnd;

  // No need to re-add devices if this is an application exit request
  if (reason == WindowChangeReason::Exit)
    ClearDevices();
  else
    RefreshDevices(RefreshReason::WindowChangeOnly);
}

void ControllerInterface::RefreshDevices(RefreshReason reason)
{
  if (!IsInit())
    return;

  // Backends with a hotplug thread may add or remove devices meanwhile; every change is
  // one swap of the whole list, so each lands complete, before or after this refresh's.
  // If only the window changed, avoid removing and re-adding all devices.
  // Instead only refresh devices that require the window handle.
  if (reason == RefreshReason::WindowChangeOnly)
  {
    retro_atomic_fetch_add_int(&m_populating_devices_counter, 1);

    for (auto& backend : m_input_backends)
      backend->HandleWindowChange();

    if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1)
      InvokeDevicesChangedCallbacks();

    return;
  }

  retro_atomic_fetch_add_int(&m_populating_devices_counter, 1);

  // Make sure shared_ptr<Device> objects are released before repopulating.
  ClearDevices();

  // Some of these calls won't immediately populate devices, but will do it async
  // with their own PlatformPopulateDevices().
  // This means that devices might end up in different order, unless we override their priority.
  // This helps the emulation and host thread to not stall when repopulating devices for any reason.
  // Every platform that adds a device that is meant to be used as default device should try to not
  // do it async, to not risk the emulated controllers default config loading not finding a default
  // device.

  for (auto& backend : m_input_backends)
    backend->PopulateDevices();

  WiimoteReal::PopulateDevices();

  if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::PlatformPopulateDevices(const std::function<void()>& callback)
{
  if (!IsInit())
    return;

  retro_atomic_fetch_add_int(&m_populating_devices_counter, 1);

  callback();

  if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1)
    InvokeDevicesChangedCallbacks();
}

// Remove all devices and call library cleanup functions
void ControllerInterface::Shutdown()
{
  if (!IsInit())
    return;

  // Prevent additional devices from being added during shutdown.
  retro_atomic_store_release_int(&m_is_init, 0);
  // Additional safety measure to avoid InvokeDevicesChangedCallbacks()
  retro_atomic_store_release_int(&m_populating_devices_counter, 1);

  // Update control references so shared_ptr<Device>s are freed up BEFORE we shutdown the backends.
  ClearDevices();

  // Empty the container of input backends to deconstruct and deinitialize them.
  m_input_backends.clear();

  // Make sure no devices had been added within Shutdown() in the time
  // between checking they checked atomic m_is_init bool and we changed it.
  // Note that this is still not 100% safe as some backends are
  // shut down in other places, possibly adding devices after we have shut down, but the chances of
  // that happening are basically zero.
  ClearDevices();
}

void ControllerInterface::ClearDevices()
{
  const bool cleared = EditDevices([](std::vector<std::shared_ptr<ciface::Core::Device>>& devices) {
    if (devices.empty())
      return false;

    for (const auto& d : devices)
    {
      // Set outputs to ZERO before destroying device
      for (ciface::Core::Device::Output* o : d->Outputs())
        o->SetState(0);
    }

    // Devices could still be alive after this as there might be shared ptrs around holding them.
    // The InvokeDevicesChangedCallbacks() underneath should always clean all of them (it needs to).
    devices.clear();
    return true;
  });

  if (cleared)
    InvokeDevicesChangedCallbacks();
}

bool ControllerInterface::AddDevice(std::shared_ptr<ciface::Core::Device> device)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!IsInit())
    return false;

  ASSERT_MSG(CONTROLLERINTERFACE, !tls_is_updating_devices,
             "Devices shouldn't be added within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  EditDevices([&device](std::vector<std::shared_ptr<ciface::Core::Device>>& devices) {
    const auto is_id_in_use = [&device, &devices](int id) {
      return std::ranges::any_of(devices, [&device, &id](const auto& d) {
        return d->GetSource() == device->GetSource() && d->GetName() == device->GetName() &&
               d->GetId() == id;
      });
    };

    const auto preferred_id = device->GetPreferredId();
    if (preferred_id.has_value() && !is_id_in_use(*preferred_id))
    {
      // Use the device's preferred ID if available.
      device->SetId(*preferred_id);
    }
    else
    {
      // Find the first available ID to use.
      int id = 0;
      while (is_id_in_use(id))
        ++id;

      device->SetId(id);
    }

    devices.emplace_back(device);

    // We can't (and don't want) to control the order in which devices are added, but we need
    // their order to be consistent, and we need the same one to always be the first, where present
    // (the keyboard and mouse device usually). This is because when defaulting a controller
    // profile, it will automatically select the first device in the list as its default. It would
    // be nice to sort devices by Source then Name then ID, but it's better to leave them sorted by
    // the add order. This also avoids breaking the order on other platforms that are less tested.
    std::ranges::stable_sort(devices, std::ranges::greater{},
                             &ciface::Core::Device::GetSortPriority);
    return true;
  });
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Added device: {}", device->GetQualifiedName());

  if (!retro_atomic_load_acquire_int(&m_populating_devices_counter))
    InvokeDevicesChangedCallbacks();
  return true;
}

void ControllerInterface::RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback,
                                       bool force_devices_release)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!IsInit())
    return;

  ASSERT_MSG(CONTROLLERINTERFACE, !tls_is_updating_devices,
             "Devices shouldn't be removed within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  const bool any_removed =
      EditDevices([&callback](std::vector<std::shared_ptr<ciface::Core::Device>>& devices) {
        return std::erase_if(devices, [&callback](const auto& dev) {
                 if (callback(dev.get()))
                 {
                   NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Removed device: {}",
                                  dev->GetQualifiedName());
                   return true;
                 }
                 return false;
               }) != 0;
      });

  if (any_removed &&
      (!retro_atomic_load_acquire_int(&m_populating_devices_counter) || force_devices_release))
    InvokeDevicesChangedCallbacks();
}

// Update input for all devices.
void ControllerInterface::UpdateInput()
{
  // This should never happen
  ASSERT(IsInit());
  if (!IsInit())
    return;

  // The devices to remove are collected as weak pointers: they are not kept alive by this, and
  // if somebody else removed them meanwhile, we'll know which ones are gone.
  std::vector<std::weak_ptr<ciface::Core::Device>> devices_to_remove;

  {
    ciface::InputReadScope scope;

    tls_is_updating_devices = true;

    for (auto& backend : m_input_backends)
      backend->UpdateInput(devices_to_remove);

    static const std::vector<std::shared_ptr<ciface::Core::Device>> s_no_devices;
    const DeviceList* const list = PeekDevices();
    for (const auto& d : list ? list->devices : s_no_devices)
    {
      // Theoretically we could avoid updating input on devices that don't have any references to
      // them, but in practice a few devices types could break in different ways, so we don't
      if (d->UpdateInput() == ciface::Core::DeviceRemoval::Remove)
        devices_to_remove.push_back(d);
    }

    tls_is_updating_devices = false;
  }

  if (devices_to_remove.size() > 0)
  {
    RemoveDevice([&](const ciface::Core::Device* device) {
      return std::ranges::any_of(devices_to_remove,
                                 [device](const auto& d) { return d.lock().get() == device; });
    });
  }
}

void ControllerInterface::SetCurrentInputChannel(ciface::InputChannel input_channel)
{
  tls_input_channel = input_channel;
}

ciface::InputChannel ControllerInterface::GetCurrentInputChannel()
{
  return tls_input_channel;
}

WindowSystemInfo ControllerInterface::GetWindowSystemInfo() const
{
  return m_wsi;
}

void ControllerInterface::SetAspectRatioAdjustment(float value)
{
  int bits;
  std::memcpy(&bits, &value, sizeof(bits));
  retro_atomic_store_release_int(&m_aspect_ratio_adjustment, bits);
}

Common::Vec2 ControllerInterface::GetWindowInputScale() const
{
  const int bits = retro_atomic_load_acquire_int(&m_aspect_ratio_adjustment);
  float ar;
  std::memcpy(&ar, &bits, sizeof(ar));

  if (ar > 1)
    return {1.f, ar};
  else
    return {1 / ar, 1.f};
}

void ControllerInterface::SetMouseCenteringRequested(bool center)
{
  retro_atomic_store_release_int(&m_requested_mouse_centering, center ? 1 : 0);
}

bool ControllerInterface::IsMouseCenteringRequested() const
{
  return retro_atomic_load_acquire_int(&m_requested_mouse_centering) != 0;
}

// Register a callback to be called when a device is added or removed (as from the input backends'
// hotplug thread), or when devices are refreshed
// Returns a handle for later removing the callback.

Common::EventHook
ControllerInterface::RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback)
{
  return m_devices_changed_event.Register(std::move(callback));
}

// Invoke all callbacks that were registered
void ControllerInterface::InvokeDevicesChangedCallbacks()
{
  m_devices_changed_event.Trigger();
}
