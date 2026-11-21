// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"


ControllerInterface g_controller_interface;

// We need to save which input channel we are in by thread, so we can access the correct input
// update values in different threads by input channel. We start from InputChannel::Host on all
// threads as hotkeys are updated from a worker thread, but UI can read from the main thread. This
// will never interfere with game threads.

static thread_local bool tls_is_updating_devices = false;

void ControllerInterface::Initialize()
{
  if (IsInit())
    return;

  retro_atomic_store_release_int(&m_populating_devices_counter, 1);

  // Don't allow backends to add devices before the first RefreshDevices() as they will be cleaned
  // there. Or they'd end up waiting on the devices mutex if populated from another thread.
  retro_atomic_store_release_int(&m_is_init, 1);

  RefreshDevices();

  bool devices_empty;
  {
    ciface::InputReadScope scope;
    const DeviceList* const list = PeekDevices();
    devices_empty = !list || list->count == 0;
  }

  if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1 && !devices_empty)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::RefreshDevices()
{
  if (!IsInit())
    return;

  // Backends with a hotplug thread may add or remove devices meanwhile; every change is
  // one swap of the whole list, so each lands complete, before or after this refresh's.
  retro_atomic_fetch_add_int(&m_populating_devices_counter, 1);

  // Make sure the devices are released before repopulating.
  ClearDevices();

  // Some of these calls won't immediately populate devices, but will do it async
  // with their own PlatformPopulateDevices().
  // This means that devices might end up in different order, unless we override their priority.
  // This helps the emulation and host thread to not stall when repopulating devices for any reason.
  // Every platform that adds a device that is meant to be used as default device should try to not
  // do it async, to not risk the emulated controllers default config loading not finding a default
  // device.

  WiimoteReal::PopulateDevices();

  if (retro_atomic_fetch_sub_int(&m_populating_devices_counter, 1) == 1)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::PlatformPopulateDevices(void (*populate)(void))
{
  if (!IsInit())
    return;

  retro_atomic_fetch_add_int(&m_populating_devices_counter, 1);

  populate();

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

  // Update control references so the devices are freed up BEFORE we shutdown the backends.
  ClearDevices();

  // Make sure no devices had been added within Shutdown() in the time
  // between checking they checked atomic m_is_init bool and we changed it.
  // Note that this is still not 100% safe as some backends are
  // shut down in other places, possibly adding devices after we have shut down, but the chances of
  // that happening are basically zero.
  ClearDevices();
}

void ControllerInterface::ClearDevices()
{
  const bool cleared = EditDevices([](DeviceList* list) {
    if (list->count == 0)
      return false;

    for (u32 d = 0; d < list->count; d++)
    {
      // Set outputs to ZERO before destroying device
      ciface::Core::Device* const device = list->devices[d];
      int i;
      for (i = 0; i < device->NumOutputs(); i++)
        device->Outputs()[i]->SetState(0);
    }

    // Devices could still be alive after this as bound controls may still hold them.
    // The InvokeDevicesChangedCallbacks() underneath should always clean all of them (it needs to).
    for (u32 d = 0; d < list->count; d++)
      list->devices[d]->Release();
    list->count = 0;
    return true;
  });

  if (cleared)
    InvokeDevicesChangedCallbacks();
}

bool ControllerInterface::AddDevice(ciface::Core::Device* device)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!IsInit())
  {
    device->Release();
    return false;
  }

  ASSERT_MSG(CONTROLLERINTERFACE, !tls_is_updating_devices,
             "Devices shouldn't be added within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  bool added = false;
  EditDevices([device, &added](DeviceList* list) {
    const auto is_id_in_use = [device, list](int id) {
      for (u32 i = 0; i < list->count; i++)
      {
        const ciface::Core::Device* const d = list->devices[i];
        if (!strcmp(d->GetSource(), device->GetSource()) &&
            !strcmp(d->GetName(), device->GetName()) && d->GetId() == id)
        {
          return true;
        }
      }
      return false;
    };

    added = false;
    if (list->count == MAX_DEVICES)
    {
      ERROR_LOG(CONTROLLERINTERFACE, "Too many input devices");
      return false;
    }

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

    // We can't (and don't want) to control the order in which devices are added, but we need
    // their order to be consistent, and we need the same one to always be the first, where present
    // (the keyboard and mouse device usually). This is because when defaulting a controller
    // profile, it will automatically select the first device in the list as its default. It would
    // be nice to sort devices by Source then Name then ID, but it's better to leave them sorted by
    // the add order. This also avoids breaking the order on other platforms that are less tested.
    // So it goes after every device of its sort priority or above.
    const int priority = device->GetSortPriority();
    u32 slot = list->count;
    while (slot > 0 && list->devices[slot - 1]->GetSortPriority() < priority)
      slot--;
    for (u32 i = list->count; i > slot; i--)
      list->devices[i] = list->devices[i - 1];
    device->AddRef();
    list->devices[slot] = device;
    list->count++;
    added = true;
    return true;
  });
  if (!added)
  {
    device->Release();
    return false;
  }
  NOTICE_LOG(CONTROLLERINTERFACE, "Added device: %s/%d/%s", device->GetSource(), device->GetId(),
             device->GetName());
  // the list holds it now
  device->Release();

  if (!retro_atomic_load_acquire_int(&m_populating_devices_counter))
    InvokeDevicesChangedCallbacks();
  return true;
}

void ControllerInterface::RemoveDevice(bool (*should_remove)(const ciface::Core::Device* device,
                                                             void* context),
                                       void* context, bool force_devices_release)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!IsInit())
    return;

  ASSERT_MSG(CONTROLLERINTERFACE, !tls_is_updating_devices,
             "Devices shouldn't be removed within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  const bool any_removed = EditDevices([should_remove, context](DeviceList* list) {
    u32 kept = 0;
    for (u32 i = 0; i < list->count; i++)
    {
      ciface::Core::Device* const dev = list->devices[i];
      if (should_remove(dev, context))
      {
        NOTICE_LOG(CONTROLLERINTERFACE, "Removed device: %s/%d/%s", dev->GetSource(), dev->GetId(),
                   dev->GetName());
        dev->Release();
      }
      else
      {
        list->devices[kept++] = dev;
      }
    }
    const bool removed = kept != list->count;
    list->count = kept;
    return removed;
  });

  if (any_removed &&
      (!retro_atomic_load_acquire_int(&m_populating_devices_counter) || force_devices_release))
    InvokeDevicesChangedCallbacks();
}

// The devices that asked to be removed, each held by a reference so none is freed and its address
// reused before the removal
struct DevicesToRemove
{
  ciface::Core::Device* devices[ciface::Core::DeviceContainer::MAX_DEVICES];
  u32 count;
};

// Whether the device is among those that asked to be removed: the context
static bool IsDeviceToRemove(const ciface::Core::Device* device, void* context)
{
  const DevicesToRemove* const to_remove = static_cast<const DevicesToRemove*>(context);
  for (u32 i = 0; i < to_remove->count; i++)
  {
    if (to_remove->devices[i] == device)
      return true;
  }
  return false;
}

// Update input for all devices.
void ControllerInterface::UpdateInput()
{
  // This should never happen
  ASSERT(IsInit());
  if (!IsInit())
    return;

  // The devices to remove are collected with references of their own: if somebody else removed
  // them meanwhile, they are simply not in the list any more.
  DevicesToRemove devices_to_remove;
  devices_to_remove.count = 0;

  {
    ciface::InputReadScope scope;

    tls_is_updating_devices = true;

    const DeviceList* const list = PeekDevices();
    for (u32 i = 0; list && i < list->count; i++)
    {
      ciface::Core::Device* const d = list->devices[i];
      // Theoretically we could avoid updating input on devices that don't have any references to
      // them, but in practice a few devices types could break in different ways, so we don't
      if (d->UpdateInput() == ciface::Core::DeviceRemoval::Remove)
      {
        d->AddRef();
        devices_to_remove.devices[devices_to_remove.count++] = d;
      }
    }

    tls_is_updating_devices = false;
  }

  if (devices_to_remove.count > 0)
  {
    RemoveDevice(IsDeviceToRemove, &devices_to_remove);
    for (u32 i = 0; i < devices_to_remove.count; i++)
      devices_to_remove.devices[i]->Release();
  }
}

// Register a callback to be called when a device is added or removed (as from the input backends'
// hotplug thread), or when devices are refreshed
// Returns a handle for later removing the callback.

Common::EventHook
ControllerInterface::RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback,
                                                    void* user)
{
  return m_devices_changed_event.Register(callback, user);
}

// Invoke all callbacks that were registered
void ControllerInterface::InvokeDevicesChangedCallbacks()
{
  m_devices_changed_event.Trigger();
}
