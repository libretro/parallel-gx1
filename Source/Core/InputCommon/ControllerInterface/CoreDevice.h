// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <retro_atomic.h>

#include "Common/CommonTypes.h"
#include "Common/Quiesce.h"

// idk in case I wanted to change it to double or something, idk what's best
typedef double ControlState;

namespace ciface
{
// Everything the input code shares between threads (the device list, control bindings,
// stick calibration) is published whole and freed through this quiesce, so reading input
// takes no lock and a reconfiguration never waits for a reader.
quiesce_t* InputQuiesce();

// Marks the calling thread as reading input for its lifetime. Nests.
class InputReadScope final
{
public:
  InputReadScope() { quiesce_enter(InputQuiesce()); }
  ~InputReadScope() { quiesce_exit(InputQuiesce()); }
  InputReadScope(const InputReadScope&) = delete;
  InputReadScope& operator=(const InputReadScope&) = delete;
};

// 100Hz which homebrew docs very roughly imply is within WiiMote normal
// range, used for periodic haptic effects though often ignored by devices
// TODO: Make this configurable.
constexpr int RUMBLE_PERIOD_MS = 10;

// This needs to be at least as long as the longest rumble that might ever be played.
// Too short and it's going to stop in the middle of a long effect.
// Infinite values are invalid for ramp effects and probably not sensible.
constexpr int RUMBLE_LENGTH_MS = 1000 * 10;

// All inputs (other than accel/gyro) return 1.0 as their maximum value.
// Battery inputs will almost always be mapped to the "Battery" setting which is a percentage.
// If someone actually wants to map a battery input to a regular control they can divide by 100.
// I think this is better than requiring multiplication by 100 for the most common usage.
constexpr ControlState BATTERY_INPUT_MAX_VALUE = 100.0;

namespace Core
{
enum class DeviceRemoval
{
  Remove,
  Keep,
};

class Device
{
public:
  class Input;
  class Output;

  //
  // Control
  //
  // Control includes inputs and outputs
  //
  class Control  // input or output
  {
  public:
    virtual ~Control() = default;
    /* The control's name, kept by the control for as long as it lives */
    virtual const char* GetName() const = 0;

    // May be overridden to allow multiple valid names.
    // Useful for backwards-compatible configurations when names change.
    virtual bool IsMatchingName(std::string_view name) const;

    // May be overridden to hide in UI.
    // Useful for backwards-compatible configurations when names change.
  };

  //
  // Input
  //
  // An input on a device
  //
  class Input : public Control
  {
  public:
    // Things like absolute axes/ absolute mouse position should override this to prevent
    // undesirable behavior in our mapping logic.

    // Implementations should return a value from 0.0 to 1.0 across their normal range.
    // One input should be provided for each "direction". (e.g. 2 for each axis)
    // If possible, negative values may be returned in situations where an opposing input is
    // activated. (e.g. When an underlying axis, X, is currently negative, "Axis X-", will return a
    // positive value and "Axis X+" may return a negative value.)
    // (Negative values are clamped off before they reach expression-parser or controller-emu)
    virtual ControlState GetState() const = 0;
  };

  //
  // Output
  //
  // An output on a device
  //
  class Output : public Control
  {
  public:
    ~Output() override = default;
    virtual void SetState(ControlState state) = 0;
  };

  Device() = default;
  virtual ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // A device is counted: made with one reference, it takes another for each holder (a device
  // list, a bound control) and is freed with the last.
  void AddRef() { retro_atomic_fetch_add_int(&m_refs, 1); }
  void Release()
  {
    if (retro_atomic_fetch_sub_int(&m_refs, 1) == 1)
      delete this;
  }

  int GetId() const { return m_id; }
  void SetId(int id) { m_id = id; }
  /* The device's name and its backend's, kept for as long as the device lives */
  virtual const char* GetName() const = 0;
  virtual const char* GetSource() const = 0;
  virtual DeviceRemoval UpdateInput() { return DeviceRemoval::Keep; }

  // May be overridden to implement hotplug removal.
  // Currently handled on a per-backend basis but this could change.
  virtual bool IsValid() const { return true; }

  // Returns true whether this device is "virtual/emulated", not linked
  // to any actual physical device. Mostly used by keyboard and mouse devices,
  // and to avoid uselessly recreating the device unless really necessary.
  // Doesn't necessarily need to be set to true if the device is virtual.

  // (e.g. Xbox 360 controllers have controller number LEDs which should match the ID we use.)

  // Use this to change the order in which devices are sorted in their list.
  // A higher priority means it will be one of the first ones (smaller index), making it more
  // likely to be index 0, which is automatically set as the default device when there isn't one.
  // Every platform should have at least one device with priority >= 0.
  static constexpr int DEFAULT_DEVICE_SORT_PRIORITY = std::numeric_limits<int>::max();
  virtual int GetSortPriority() const { return 0; }

  Output* const* Outputs() const { return m_outputs; }
  int NumOutputs() const { return m_num_outputs; }

  Input* FindInput(std::string_view name) const;
  Output* FindOutput(std::string_view name) const;

protected:
  void AddInput(Input* const i);
  void AddOutput(Output* const o);

private:
  retro_atomic_int_t m_refs{1};
  int m_id = 0;
  // Owned, the first m_num_inputs and m_num_outputs of them: made with the device
  Input** m_inputs = nullptr;
  int m_num_inputs = 0;
  int m_cap_inputs = 0;
  Output** m_outputs = nullptr;
  int m_num_outputs = 0;
  int m_cap_outputs = 0;
};

//
// DeviceQualifier
//
// Device qualifier used to match devices.
// Currently has ( source, id, name ) properties which match a device
//
class DeviceQualifier
{
public:
  DeviceQualifier() : cid(-1) {}
  DeviceQualifier(std::string source_, const int id_, std::string name_)
      : source(std::move(source_)), cid(id_), name(std::move(name_))
  {
  }
  void FromDevice(const Device* const dev);
  // From "source/cid/name": len characters, not terminated
  void FromString(const char* str, std::size_t len);
  // Writes "source/cid/name" terminated, as much as fits in size; the length of all of it, which
  // is 0 for a qualifier of nothing.
  std::size_t ToString(char* buf, std::size_t size) const;

  bool operator==(const DeviceQualifier& devq) const;

  bool operator==(const Device* dev) const;

  std::string source;
  int cid;
  std::string name;
};

class DeviceContainer
{
public:
  // The most devices listed at once
  static constexpr u32 MAX_DEVICES = 64;

  Device::Input* FindInput(std::string_view name, const Device* def_dev) const;
  Device::Output* FindOutput(std::string_view name, const Device* def_dev) const;

  /* Calls fn(device, ctx) for every device, without copying the list. */
  void ForEachDevice(void (*fn)(Device* device, void* ctx), void* ctx) const
  {
    InputReadScope scope;
    const DeviceList* const list = PeekDevices();
    size_t i;
    if (!list)
      return;
    for (i = 0; i < list->count; i++)
      fn(list->devices[i], ctx);
  }
  // The qualifier of the device to bind by default; false when there is none.
  bool GetDefaultDevice(DeviceQualifier* qualifier) const;
  // The device matching devq with a reference the caller releases; nullptr when there is none
  Device* FindDevice(const DeviceQualifier& devq) const;

  bool HasConnectedDevice(const DeviceQualifier& qualifier) const;

  DeviceContainer() = default;
  ~DeviceContainer();
  DeviceContainer(const DeviceContainer&) = delete;
  DeviceContainer& operator=(const DeviceContainer&) = delete;

protected:
  // The devices, sorted by priority. Never changed once published: a writer copies it,
  // edits the copy and swaps it in, and the old one is freed once no reader is inside.
  // Each device in a list is held by a reference of the list's.
  struct DeviceList
  {
    quiesce_node_t node;  // first: the free callback casts it back
    u32 count;
    Device* devices[MAX_DEVICES];
  };

  // The current list, or null for none. Only inside an InputReadScope.
  const DeviceList* PeekDevices() const
  {
    return static_cast<const DeviceList*>(retro_atomic_load_acquire_ptr(&m_device_list));
  }

  // Swaps in edit's result, made from a copy of the current devices. edit returns false to
  // leave the list as it is; it may run more than once if another thread swaps in a list
  // meanwhile. Returns whether a list was swapped in. A device edit puts in takes a reference
  // for the list; one it takes out has the list's reference released.
  template <typename Edit>
  bool EditDevices(Edit&& edit)
  {
    InputReadScope scope;
    for (;;)
    {
      void* const cur = retro_atomic_load_acquire_ptr(&m_device_list);
      DeviceList* const next = new DeviceList{};
      if (cur)
      {
        const DeviceList* const current = static_cast<const DeviceList*>(cur);
        next->count = current->count;
        for (u32 i = 0; i < next->count; i++)
        {
          next->devices[i] = current->devices[i];
          next->devices[i]->AddRef();
        }
      }
      if (!edit(next))
      {
        FreeDeviceList(&next->node);
        return false;
      }
      if (retro_atomic_cas_ptr(&m_device_list, cur, next))
      {
        if (cur)
          quiesce_retire(InputQuiesce(), &static_cast<DeviceList*>(cur)->node, FreeDeviceList);
        return true;
      }
      FreeDeviceList(&next->node);
    }
  }

private:
  static void FreeDeviceList(quiesce_node_t* node);

  mutable retro_atomic_ptr_t m_device_list{};
};

}  // namespace Core
}  // namespace ciface
