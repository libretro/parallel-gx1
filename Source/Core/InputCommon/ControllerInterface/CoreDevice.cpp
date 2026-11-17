// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string.h>
#include <string>
#include <tuple>
#include <utility>

#include "Common/MathUtil.h"

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
  const char* GetName() const override { return m_name.c_str(); }
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
  return std::string_view(GetName()) == name;
}

class FullAnalogSurface final : public Device::Input
{
public:
  FullAnalogSurface(Input* low, Input* high) : m_low(*low), m_high(*high)
  {
    const char* const high_name = m_high.GetName();
    const size_t high_length = strlen(high_name);
    /* e.g. "Full Axis X+" */
    snprintf(m_name, sizeof(m_name), "Full %s", high_name);
    /* the old naming scheme, "Axis X-+": the low input's name and the high one's sign */
    snprintf(m_old_name, sizeof(m_old_name), "%s%c", m_low.GetName(),
             high_length ? high_name[high_length - 1] : '\0');
  }

  ControlState GetState() const override
  {
    return (1 + std::max(0.0, m_high.GetState()) - std::max(0.0, m_low.GetState())) / 2;
  }

  const char* GetName() const override { return m_name; }

  bool IsMatchingName(std::string_view name) const override
  {
    if (Control::IsMatchingName(name))
      return true;

    // Old naming scheme was "Axis X-+" which is too visually similar to "Axis X+".
    // This has caused countless problems for users with mysterious misconfigurations.
    // We match this old name to support old configurations.
    return std::string_view(m_old_name) == name;
  }

private:
  Input& m_low;
  Input& m_high;
  char m_name[64];
  char m_old_name[64];
};

//
// DeviceQualifier :: ToString
//
// Get string from a device qualifier / serialize
//
std::size_t DeviceQualifier::ToString(char* buf, std::size_t size) const
{
  int len;

  if (source.empty() && (cid < 0) && name.empty())
  {
    if (size)
      buf[0] = '\0';
    return 0;
  }

  if (cid > -1)
    len = snprintf(buf, size, "%s/%d/%s", source.c_str(), cid, name.c_str());
  else
    len = snprintf(buf, size, "%s//%s", source.c_str(), name.c_str());
  return len > 0 ? static_cast<std::size_t>(len) : 0;
}

//
// DeviceQualifier :: FromString
//
// Set a device qualifier from a string / unserialize
//
void DeviceQualifier::FromString(const char* str, std::size_t len)
{
  const char* const end = str + len;
  const char* first;
  const char* second;
  const char* p;
  unsigned value = 0;
  bool negative = false;

  // "source/cid/name"; the name may itself contain '/'
  first = static_cast<const char*>(memchr(str, '/', len));
  source.assign(str, first ? static_cast<std::size_t>(first - str) : len);
  cid = -1;
  name.clear();
  if (!first)
    return;
  second =
      static_cast<const char*>(memchr(first + 1, '/', static_cast<std::size_t>(end - first - 1)));
  if (!second)
    second = end;

  // the cid as atoi reads it: after whitespace, a sign, then the digits
  p = first + 1;
  while (p < second && (*p == ' ' || (*p >= '\t' && *p <= '\r')))
    p++;
  if (p < second && (*p == '+' || *p == '-'))
    negative = *p++ == '-';
  while (p < second && *p >= '0' && *p <= '9')
    value = value * 10 + static_cast<unsigned>(*p++ - '0');
  cid = static_cast<int>(negative ? 0u - value : value);

  if (second != end)
    name.assign(second + 1, static_cast<std::size_t>(end - second - 1));
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
    if (!strcmp(dev->GetName(), name.c_str()))
      if (!strcmp(dev->GetSource(), source.c_str()))
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

bool DeviceContainer::GetDefaultDevice(DeviceQualifier* qualifier) const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  const auto& devices = list ? list->devices : NoDevices();
  // Devices are already sorted by priority
  if (devices.empty() || devices[0]->GetSortPriority() < 0)
    return false;

  qualifier->FromDevice(devices[0].get());
  return true;
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
