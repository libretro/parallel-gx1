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

Device::Input* Device::FindInput(const char* name) const
{
  int i;
  for (i = 0; i < m_num_inputs; i++)
  {
    if (!strcmp(m_inputs[i]->GetName(), name))
      return m_inputs[i];
  }

  return nullptr;
}

Device::Output* Device::FindOutput(const char* name) const
{
  int i;
  for (i = 0; i < m_num_outputs; i++)
  {
    if (!strcmp(m_outputs[i]->GetName(), name))
      return m_outputs[i];
  }

  return nullptr;
}

//
// DeviceQualifier :: ToString
//
// Get string from a device qualifier / serialize
//
std::size_t DeviceQualifier::ToString(char* buf, std::size_t size) const
{
  int len;

  if (!source[0] && (cid < 0) && !name[0])
  {
    if (size)
      buf[0] = '\0';
    return 0;
  }

  if (cid > -1)
    len = snprintf(buf, size, "%s/%d/%s", source, cid, name);
  else
    len = snprintf(buf, size, "%s//%s", source, name);
  return len > 0 ? static_cast<std::size_t>(len) : 0;
}

//
// DeviceQualifier :: FromString
//
// Set a device qualifier from a string / unserialize
//
// The len characters of text, terminated, as much as fits in the buffer of size characters
static void AssignText(char* buffer, std::size_t size, const char* text, std::size_t len)
{
  if (len >= size)
    len = size - 1;
  memcpy(buffer, text, len);
  buffer[len] = '\0';
}

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
  AssignText(source, sizeof(source), str, first ? static_cast<std::size_t>(first - str) : len);
  cid = -1;
  name[0] = '\0';
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
    AssignText(name, sizeof(name), second + 1, static_cast<std::size_t>(end - second - 1));
}

//
// DeviceQualifier :: FromDevice
//
// Set a device qualifier from a device
//
void DeviceQualifier::FromDevice(const Device* const dev)
{
  AssignText(name, sizeof(name), dev->GetName(), strlen(dev->GetName()));
  cid = dev->GetId();
  AssignText(source, sizeof(source), dev->GetSource(), strlen(dev->GetSource()));
}

bool DeviceQualifier::operator==(const Device* const dev) const
{
  if (dev->GetId() == cid)
    if (!strcmp(dev->GetName(), name))
      if (!strcmp(dev->GetSource(), source))
        return true;

  return false;
}

bool DeviceQualifier::operator==(const DeviceQualifier& devq) const
{
  return cid == devq.cid && !strcmp(name, devq.name) && !strcmp(source, devq.source);
}

void DeviceContainer::FreeDeviceList(quiesce_node_t* node)
{
  DeviceList* const list = reinterpret_cast<DeviceList*>(node);
  for (u32 i = 0; i < list->count; i++)
    list->devices[i]->Release();
  delete list;
}

DeviceContainer::~DeviceContainer()
{
  void* const list = retro_atomic_exchange_ptr(&m_device_list, nullptr);
  if (list)
    FreeDeviceList(&static_cast<DeviceList*>(list)->node);
}

Device* DeviceContainer::FindDevice(const DeviceQualifier& devq) const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  for (u32 i = 0; list && i < list->count; i++)
  {
    if (devq == list->devices[i])
    {
      list->devices[i]->AddRef();
      return list->devices[i];
    }
  }

  return nullptr;
}

bool DeviceContainer::GetDefaultDevice(DeviceQualifier* qualifier) const
{
  InputReadScope scope;
  const DeviceList* const list = PeekDevices();
  // Devices are already sorted by priority
  if (!list || list->count == 0 || list->devices[0]->GetSortPriority() < 0)
    return false;

  qualifier->FromDevice(list->devices[0]);
  return true;
}

bool DeviceContainer::HasConnectedDevice(const DeviceQualifier& qualifier) const
{
  Device* const device = FindDevice(qualifier);
  if (!device)
    return false;
  const bool valid = device->IsValid();
  device->Release();
  return valid;
}

}  // namespace ciface::Core
