// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <memory>
#include <optional>
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
    virtual std::string GetName() const = 0;

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
    // Doing so is solely to allow our input detection logic to better detect false positives.
    // This is necessary when making use of "FullAnalogSurface" as multiple inputs will be seen
    // increasing from 0.0 to 1.0 as a user tries to map just one. The negative values provide a
    // view of the underlying axis. (Negative values are clamped off before they reach
    // expression-parser or controller-emu)
    virtual ControlState GetState() const = 0;

    // Overridden by CombinedInput,
    // so hotkey logic knows Ctrl, L_Ctrl, and R_Ctrl are the same,
    // and so input detection can return the parent name.
    virtual bool IsChild(const Input*) const { return false; }
  };

  class RelativeInput : public Input
  {
  public:
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

  virtual ~Device();

  int GetId() const { return m_id; }
  void SetId(int id) { m_id = id; }
  virtual std::string GetName() const = 0;
  virtual std::string GetSource() const = 0;
  std::string GetQualifiedName() const;
  virtual DeviceRemoval UpdateInput() { return DeviceRemoval::Keep; }

  // May be overridden to implement hotplug removal.
  // Currently handled on a per-backend basis but this could change.
  virtual bool IsValid() const { return true; }

  // Returns true whether this device is "virtual/emulated", not linked
  // to any actual physical device. Mostly used by keyboard and mouse devices,
  // and to avoid uselessly recreating the device unless really necessary.
  // Doesn't necessarily need to be set to true if the device is virtual.

  // (e.g. Xbox 360 controllers have controller number LEDs which should match the ID we use.)
  virtual std::optional<int> GetPreferredId() const;

  // Use this to change the order in which devices are sorted in their list.
  // A higher priority means it will be one of the first ones (smaller index), making it more
  // likely to be index 0, which is automatically set as the default device when there isn't one.
  // Every platform should have at least one device with priority >= 0.
  static constexpr int DEFAULT_DEVICE_SORT_PRIORITY = std::numeric_limits<int>::max();
  virtual int GetSortPriority() const { return 0; }

  const std::vector<Input*>& Inputs() const { return m_inputs; }
  const std::vector<Output*>& Outputs() const { return m_outputs; }

  Input* FindInput(std::string_view name) const;
  Output* FindOutput(std::string_view name) const;

protected:
  void AddInput(Input* const i);
  void AddOutput(Output* const o);

  // Pass Inputs for center-neutral (- and +) directions of some axis.
  // This function adds those Inputs and also a FullAnalogSurface Input for each direction.
  // This is only needed when it's not known if the particular axis is neutral in the center
  //  or neutral on one of the extremes.
  // Some e.g. DInput devices expose a trigger across the full analog surface
  //  but we have no way of knowing this until the user actually maps the Input,
  //  so both center-neutral and full-surface Inputs need to be created in that case.

private:
  int m_id = 0;
  std::vector<Input*> m_inputs;
  std::vector<Output*> m_outputs;
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
  void FromString(const std::string& str);
  std::string ToString() const;

  bool operator==(const DeviceQualifier& devq) const;

  bool operator==(const Device* dev) const;

  std::string source;
  int cid;
  std::string name;
};

class DeviceContainer
{
public:
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
    for (i = 0; i < list->devices.size(); i++)
      fn(list->devices[i].get(), ctx);
  }
  std::vector<std::string> GetAllDeviceStrings() const;
  std::string GetDefaultDeviceString() const;
  std::shared_ptr<Device> FindDevice(const DeviceQualifier& devq) const;

  bool HasConnectedDevice(const DeviceQualifier& qualifier) const;

  DeviceContainer() = default;
  ~DeviceContainer();
  DeviceContainer(const DeviceContainer&) = delete;
  DeviceContainer& operator=(const DeviceContainer&) = delete;

protected:
  // The devices, sorted by priority. Never changed once published: a writer copies it,
  // edits the copy and swaps it in, and the old one is freed once no reader is inside.
  struct DeviceList
  {
    quiesce_node_t node;  // first: the free callback casts it back
    std::vector<std::shared_ptr<Device>> devices;
  };

  // The current list, or null for none. Only inside an InputReadScope.
  const DeviceList* PeekDevices() const
  {
    return static_cast<const DeviceList*>(retro_atomic_load_acquire_ptr(&m_device_list));
  }

  // Swaps in edit's result, made from a copy of the current devices. edit returns false to
  // leave the list as it is; it may run more than once if another thread swaps in a list
  // meanwhile. Returns whether a list was swapped in.
  template <typename Edit>
  bool EditDevices(Edit&& edit)
  {
    InputReadScope scope;
    for (;;)
    {
      void* const cur = retro_atomic_load_acquire_ptr(&m_device_list);
      auto* next = new DeviceList{{}, {}};
      if (cur)
        next->devices = static_cast<const DeviceList*>(cur)->devices;
      if (!edit(next->devices))
      {
        delete next;
        return false;
      }
      if (retro_atomic_cas_ptr(&m_device_list, cur, next))
      {
        if (cur)
          quiesce_retire(InputQuiesce(), &static_cast<DeviceList*>(cur)->node, FreeDeviceList);
        return true;
      }
      delete next;
    }
  }

private:
  static void FreeDeviceList(quiesce_node_t* node);

  mutable retro_atomic_ptr_t m_device_list{};
};

}  // namespace Core
}  // namespace ciface
