// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Debug/Watches.h"


#include <fmt/format.h>

namespace Common::Debug
{
Watch::Watch(u32 address_, std::string name_, State is_enabled_)
    : address(address_), name(std::move(name_)), is_enabled(is_enabled_)
{
}

std::size_t Watches::SetWatch(u32 address, std::string name)
{
  const std::size_t size = m_watches.size();
  for (std::size_t index = 0; index < size; index++)
  {
    if (m_watches.at(index).address == address)
    {
      UpdateWatch(index, address, name);
      return index;
    }
  }
  m_watches.emplace_back(address, std::move(name), Watch::State::Enabled);
  return size;
}

const Watch& Watches::GetWatch(std::size_t index) const
{
  return m_watches.at(index);
}

const std::vector<Watch>& Watches::GetWatches() const
{
  return m_watches;
}

void Watches::UnsetWatch(u32 address)
{
  size_t kept = 0;
  for (size_t i = 0; i < m_watches.size(); i++)
  {
    if (m_watches[i].address == address)
      continue;
    if (i != kept)
      m_watches[kept] = std::move(m_watches[i]);
    kept++;
  }
  m_watches.erase(m_watches.begin() + kept, m_watches.end());
}

void Watches::UpdateWatch(std::size_t index, u32 address, std::string name)
{
  m_watches[index].address = address;
  m_watches[index].name = std::move(name);
}

void Watches::UpdateWatchAddress(std::size_t index, u32 address)
{
  m_watches[index].address = address;
}

void Watches::UpdateWatchName(std::size_t index, std::string name)
{
  m_watches[index].name = std::move(name);
}

void Watches::UpdateWatchLockedState(std::size_t index, bool locked)
{
  m_watches[index].locked = locked;
}

void Watches::EnableWatch(std::size_t index)
{
  m_watches[index].is_enabled = Watch::State::Enabled;
}

void Watches::DisableWatch(std::size_t index)
{
  m_watches[index].is_enabled = Watch::State::Disabled;
}

bool Watches::HasEnabledWatch(u32 address) const
{
  for (const auto& watch : m_watches)
  {
    if (watch.address == address && watch.is_enabled == Watch::State::Enabled)
      return true;
  }
  return false;
}

void Watches::RemoveWatch(std::size_t index)
{
  m_watches.erase(m_watches.begin() + index);
}

void Watches::Clear()
{
  m_watches.clear();
}
}  // namespace Common::Debug
