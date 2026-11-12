// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "Common/MathUtil.h"
#include "Common/StringUtil.h"

namespace ciface
{
static quiesce_t s_input_quiesce;

quiesce_t* InputQuiesce()
{
  return &s_input_quiesce;
}
}  // namespace ciface

namespace ciface::Core
{
// Compared to an input's current state (ideally 1.0) minus abs(initial_state) (ideally 0.0).
// Note: Detect() logic assumes this is greater than 0.5.
constexpr ControlState INPUT_DETECT_THRESHOLD = 0.55;

class CombinedInput final : public Device::Input
{
public:
  using Inputs = std::pair<Device::Input*, Device::Input*>;

  CombinedInput(std::string name, Inputs inputs)
      : m_name(std::move(name)), m_inputs(std::move(inputs))
  {
  }
  ControlState GetState() const override
  {
    ControlState result = 0;

    if (m_inputs.first)
      result = m_inputs.first->GetState();

    if (m_inputs.second)
      result = std::max(result, m_inputs.second->GetState());

    return result;
  }
  std::string GetName() const override { return m_name; }
  bool IsChild(const Input* input) const override
  {
    return m_inputs.first == input || m_inputs.second == input;
  }

private:
  const std::string m_name;
  const std::pair<Device::Input*, Device::Input*> m_inputs;
};

Device::~Device()
{
  // delete inputs
  for (Device::Input* input : m_inputs)
    delete input;

  // delete outputs
  for (Device::Output* output : m_outputs)
    delete output;
}

std::optional<int> Device::GetPreferredId() const
{
  return {};
}

void Device::AddInput(Device::Input* const i)
{
  m_inputs.push_back(i);
}

void Device::AddOutput(Device::Output* const o)
{
  m_outputs.push_back(o);
}

std::string Device::GetQualifiedName() const
{
  return StringFromFormat("%s/%d/%s", GetSource().c_str(), GetId(), GetName().c_str());
}

Device::Input* Device::FindInput(std::string_view name) const
{
  for (Input* input : m_inputs)
  {
    if (input->IsMatchingName(name))
      return input;
  }

  return nullptr;
}

Device::Output* Device::FindOutput(std::string_view name) const
{
  for (Output* output : m_outputs)
  {
    if (output->IsMatchingName(name))
      return output;
  }

  return nullptr;
}

bool Device::Control::IsMatchingName(std::string_view name) const
{
  return GetName() == name;
}

class FullAnalogSurface final : public Device::Input
{
public:
  FullAnalogSurface(Input* low, Input* high) : m_low(*low), m_high(*high) {}

  ControlState GetState() const override
  {
    return (1 + std::max(0.0, m_high.GetState()) - std::max(0.0, m_low.GetState())) / 2;
  }

  std::string GetName() const override
  {
    // E.g. "Full Axis X+"
    return "Full " + m_high.GetName();
  }

  bool IsMatchingName(std::string_view name) const override
  {
    if (Control::IsMatchingName(name))
      return true;

    // Old naming scheme was "Axis X-+" which is too visually similar to "Axis X+".
    // This has caused countless problems for users with mysterious misconfigurations.
    // We match this old name to support old configurations.
    const auto old_name = m_low.GetName() + *m_high.GetName().rbegin();

    return old_name == name;
  }

private:
  Input& m_low;
  Input& m_high;
};

//
// DeviceQualifier :: ToString
//
// Get string from a device qualifier / serialize
//
std::string DeviceQualifier::ToString() const
{
  if (source.empty() && (cid < 0) && name.empty())
    return {};

  if (cid > -1)
    return StringFromFormat("%s/%d/%s", source.c_str(), cid, name.c_str());
  else
    return StringFromFormat("%s//%s", source.c_str(), name.c_str());
}

//
// DeviceQualifier :: FromString
//
// Set a device qualifier from a string / unserialize
//
void DeviceQualifier::FromString(const std::string& str)
{
  *this = {};

  // "source/cid/name"; the name may itself contain '/'
  const size_t first = str.find('/');
  source = str.substr(0, first);
  if (first == std::string::npos)
    return;
  const size_t second = str.find('/', first + 1);
  cid = atoi(str.substr(first + 1, second - first - 1).c_str());
  if (second != std::string::npos)
    name = str.substr(second + 1);
}

//
// DeviceQualifier :: FromDevice
//
// Set a device qualifier from a device
//
void DeviceQualifier::FromDevice(const Device* const dev)
{
  name = dev->GetName();
  cid = dev->GetId();
  source = dev->GetSource();
}

bool DeviceQualifier::operator==(const Device* const dev) const
{
  if (dev->GetId() == cid)
    if (dev->GetName() == name)
      if (dev->GetSource() == source)
        return true;

  return false;
}

bool DeviceQualifier::operator==(const DeviceQualifier& devq) const
{
  return std::tie(cid, name, source) == std::tie(devq.cid, devq.name, devq.source);
}

static const std::vector<std::shared_ptr<Device>>& NoDevices()
{
  static const std::vector<std::shared_ptr<Device>> s_none;
  return s_none;
}

void DeviceContainer::FreeDeviceList(quiesce_node_t* node)
{
  delete reinterpret_cast<DeviceList*>(node);
}

DeviceContainer::~DeviceContainer()
{
  delete static_cast<DeviceList*>(retro_atomic_exchange_ptr(&m_device_list, nullptr));
}

std::shared_ptr<Device> DeviceContainer::FindDevice(const DeviceQualifier& devq) const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  const auto& devices = list ? list->devices : NoDevices();
  for (const auto& d : devices)
  {
    if (devq == d.get())
      return d;
  }

  return nullptr;
}

std::vector<std::string> DeviceContainer::GetAllDeviceStrings() const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  const auto& devices = list ? list->devices : NoDevices();

  std::vector<std::string> device_strings;
  DeviceQualifier device_qualifier;

  for (const auto& d : devices)
  {
    device_qualifier.FromDevice(d.get());
    device_strings.emplace_back(device_qualifier.ToString());
  }

  return device_strings;
}

std::string DeviceContainer::GetDefaultDeviceString() const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  const auto& devices = list ? list->devices : NoDevices();
  // Devices are already sorted by priority
  if (devices.empty() || devices[0]->GetSortPriority() < 0)
    return "";

  DeviceQualifier device_qualifier;
  device_qualifier.FromDevice(devices[0].get());
  return device_qualifier.ToString();
}

Device::Input* DeviceContainer::FindInput(std::string_view name, const Device* def_dev) const
{
  if (def_dev)
  {
    Device::Input* const inp = def_dev->FindInput(name);
    if (inp)
      return inp;
  }

  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  const auto& devices = list ? list->devices : NoDevices();
  for (const auto& d : devices)
  {
    Device::Input* const i = d->FindInput(name);

    if (i)
      return i;
  }

  return nullptr;
}

Device::Output* DeviceContainer::FindOutput(std::string_view name, const Device* def_dev) const
{
  return def_dev->FindOutput(name);
}

bool DeviceContainer::HasConnectedDevice(const DeviceQualifier& qualifier) const
{
  const auto device = FindDevice(qualifier);
  return device != nullptr && device->IsValid();
}

}  // namespace ciface::Core
