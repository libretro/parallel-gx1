// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Config/Layer.h"

#include <cstdlib>
#include <cstring>

#include "Common/CommonFuncs.h"

namespace Config
{
ConfigLayerLoader::ConfigLayerLoader(LayerType layer) : m_layer(layer)
{
}

ConfigLayerLoader::~ConfigLayerLoader() = default;

LayerType ConfigLayerLoader::GetLayer() const
{
  return m_layer;
}

Layer::Layer(LayerType type) : m_layer(type)
{
}

Layer::Layer(ConfigLayerLoader* loader) : m_layer(loader->GetLayer()), m_loader(loader)
{
  Load();
}

Layer::~Layer()
{
  Save();
  delete m_loader;
  for (u32 i = 0; i < m_entry_count; i++)
  {
    std::free(m_entries[i].section);
    std::free(m_entries[i].key);
    std::free(m_entries[i].value);
  }
  std::free(m_entries);
}

// A copy of string on the heap, freed with free(); nullptr when out of memory
static char* CopyString(const char* string)
{
  const size_t size = std::strlen(string) + 1;
  char* const copy = static_cast<char*>(std::malloc(size));
  if (copy)
    std::memcpy(copy, string, size);
  return copy;
}

// The order of Location::operator<
static int CompareEntry(const LayerEntry& entry, const Location& location)
{
  if (entry.system != location.system)
    return entry.system < location.system ? -1 : 1;
  const int section_compare = strcasecmp(entry.section, location.section.c_str());
  if (section_compare != 0)
    return section_compare;
  return strcasecmp(entry.key, location.key.c_str());
}

u32 Layer::LowerBound(const Location& location, bool* found) const
{
  u32 low = 0;
  u32 high = m_entry_count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (CompareEntry(m_entries[mid], location) < 0)
      low = mid + 1;
    else
      high = mid;
  }
  *found = low < m_entry_count && CompareEntry(m_entries[low], location) == 0;
  return low;
}

const LayerEntry* Layer::FindEntry(const Location& location) const
{
  bool found;
  const u32 index = LowerBound(location, &found);
  return found ? &m_entries[index] : nullptr;
}

bool Layer::Exists(const Location& location) const
{
  const LayerEntry* const entry = FindEntry(location);
  return entry && entry->value;
}

bool Layer::DeleteKey(const Location& location)
{
  m_is_dirty = true;
  bool found;
  const u32 index = LowerBound(location, &found);
  if (!found || !m_entries[index].value)
    return false;

  std::free(m_entries[index].value);
  m_entries[index].value = nullptr;
  return true;
}

bool Layer::Set(const Location& location, const char* new_value)
{
  bool found;
  const u32 index = LowerBound(location, &found);
  if (found && m_entries[index].value && !std::strcmp(m_entries[index].value, new_value))
    return false;

  char* const value = CopyString(new_value);
  if (!value)
    return false;
  m_is_dirty = true;

  // a key already seen keeps the spelling it was first given
  if (found)
  {
    std::free(m_entries[index].value);
    m_entries[index].value = value;
    return true;
  }

  char* const section = CopyString(location.section.c_str());
  char* const key = CopyString(location.key.c_str());
  if (m_entry_count == m_entry_capacity && section && key)
  {
    const u32 capacity = m_entry_capacity ? m_entry_capacity * 2 : 64;
    LayerEntry* const entries =
        static_cast<LayerEntry*>(std::realloc(m_entries, capacity * sizeof(LayerEntry)));
    if (entries)
    {
      m_entries = entries;
      m_entry_capacity = capacity;
    }
  }
  if (!section || !key || m_entry_count == m_entry_capacity)
  {
    std::free(section);
    std::free(key);
    std::free(value);
    return false;
  }

  std::memmove(&m_entries[index + 1], &m_entries[index],
               (m_entry_count - index) * sizeof(LayerEntry));
  m_entries[index].system = location.system;
  m_entries[index].section = section;
  m_entries[index].key = key;
  m_entries[index].value = value;
  m_entry_count++;
  return true;
}

void Layer::Load()
{
  if (m_loader)
    m_loader->Load(this);
  m_is_dirty = false;
}

void Layer::Save()
{
  if (!m_loader || !m_is_dirty)
    return;

  m_loader->Save(this);
  m_is_dirty = false;
}

LayerType Layer::GetLayer() const
{
  return m_layer;
}
}  // namespace Config
