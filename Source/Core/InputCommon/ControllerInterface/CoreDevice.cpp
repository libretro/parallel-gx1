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
Device::~Device()
{
  int i;
  for (i = 0; i < m_num_inputs; i++)
    delete m_inputs[i];
  std::free(m_inputs);

  for (i = 0; i < m_num_outputs; i++)
    delete m_outputs[i];
  std::free(m_outputs);
}

std::optional<int> Device::GetPreferredId() const
{
  return {};
}

/* The lists grow while the device is made; a device cannot be made
 * without its controls, so running out of memory here ends the program, as
 * the vectors these were did. */
void Device::AddInput(Device::Input* const i)
{
  if (m_num_inputs == m_cap_inputs)
  {
    const int cap = m_cap_inputs ? m_cap_inputs * 2 : 32;
    Input** const grown = static_cast<Input**>(std::realloc(m_inputs, cap * sizeof(*grown)));
    if (!grown)
      std::abort();
    m_inputs = grown;
    m_cap_inputs = cap;
  }
  m_inputs[m_num_inputs++] = i;
}

void Device::AddOutput(Device::Output* const o)
{
  if (m_num_outputs == m_cap_outputs)
  {
    const int cap = m_cap_outputs ? m_cap_outputs * 2 : 4;
    Output** const grown = static_cast<Output**>(std::realloc(m_outputs, cap * sizeof(*grown)));
    if (!grown)
      std::abort();
    m_outputs = grown;
    m_cap_outputs = cap;
  }
  m_outputs[m_num_outputs++] = o;
}

Device::Input* Device::FindInput(std::string_view name) const
{
  int i;
  for (i = 0; i < m_num_inputs; i++)
  {
    if (m_inputs[i]->IsMatchingName(name))
      return m_inputs[i];
  }

  return nullptr;
}

Device::Output* Device::FindOutput(std::string_view name) const
{
  int i;
  for (i = 0; i < m_num_outputs; i++)
  {
    if (m_outputs[i]->IsMatchingName(name))
      return m_outputs[i];
  }

  return nullptr;
}

bool Device::Control::IsMatchingName(std::string_view name) const
{
  return std::string_view(GetName()) == name;
}

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
