// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <type_traits>

#include "Common/Config/ConfigInfo.h"
#include "Common/Config/Enums.h"
#include "Common/StringUtil.h"

namespace Config
{
// Setting a key to an object of this type will delete the key.
struct DefaultState
{
  friend constexpr bool operator==(DefaultState, DefaultState) { return true; }
};

namespace detail
{
// Each false, leaving out alone, when str_value does not parse as a T
template <typename T>
requires(!Common::Enum<T>)
bool TryParse(const char* str_value, T* out)
{
  T value;
  if (!::TryParse(str_value, &value))
    return false;
  *out = value;
  return true;
}

template <Common::Enum T>
bool TryParse(const char* str_value, T* out)
{
  std::underlying_type_t<T> value;
  if (!TryParse<std::underlying_type_t<T>>(str_value, &value))
    return false;
  *out = static_cast<T>(value);
  return true;
}

template <>
inline bool TryParse(const char* str_value, std::string* out)
{
  *out = str_value;
  return true;
}
}  // namespace detail

template <typename T>
class Info;

class Layer;

// A key a layer has seen, with its value, or none once deleted. The strings are owned.
struct LayerEntry
{
  System system;
  char* section;
  char* key;
  char* value;  // null once the key is deleted
};

class ConfigLayerLoader
{
public:
  explicit ConfigLayerLoader(LayerType layer);
  virtual ~ConfigLayerLoader();
  virtual void Load(Layer* config_layer) = 0;
  virtual void Save(Layer* config_layer) = 0;

  LayerType GetLayer() const;

private:
  const LayerType m_layer;
};

class Layer final
{
public:
  explicit Layer(LayerType layer);
  // takes loader
  explicit Layer(ConfigLayerLoader* loader);
  ~Layer();

  // Convenience functions
  bool Exists(const Location& location) const;
  bool DeleteKey(const Location& location);

  template <typename T>
  T Get(const Info<T>& config_info) const
  {
    T value;
    return Get<T>(config_info.GetLocation(), &value) ? value : config_info.GetDefaultValue();
  }

  // False, leaving out alone, when the layer has no parseable value there
  template <typename T>
  bool Get(const Location& location, T* out) const
  {
    const LayerEntry* const entry = FindEntry(location);
    if (!entry || !entry->value)
      return false;
    return detail::TryParse<T>(entry->value, out);
  }

  template <typename T>
  bool Set(const Info<T>& config_info, DefaultState)
  {
    return DeleteKey(config_info.GetLocation());
  }

  template <typename T>
  bool Set(const Info<T>& config_info, const std::common_type_t<T>& value)
  {
    return Set(config_info.GetLocation(), value);
  }

  bool Set(const Location& location, DefaultState) { return DeleteKey(location); }

  template <typename T>
  bool Set(const Location& location, const T& value)
  {
    return Set(location, ValueToString(value).c_str());
  }

  bool Set(const Location& location, const std::string& new_value)
  {
    return Set(location, new_value.c_str());
  }

  // Whether the value changed
  bool Set(const Location& location, const char* new_value);

  void MarkAsDirty() { m_is_dirty = true; }

  // Explicit load and save of layers
  void Load();
  void Save();

  LayerType GetLayer() const;
  // The keys the layer has seen, sorted by system, then section and key ignoring case
  const LayerEntry* GetEntries() const { return m_entries; }
  u32 GetEntryCount() const { return m_entry_count; }

private:
  // Where the entry for location is, or would be put; *found tells which
  u32 LowerBound(const Location& location, bool* found) const;
  const LayerEntry* FindEntry(const Location& location) const;

  bool m_is_dirty = false;
  LayerEntry* m_entries = nullptr;
  u32 m_entry_count = 0;
  u32 m_entry_capacity = 0;
  const LayerType m_layer;
  ConfigLayerLoader* m_loader = nullptr;  // owned
};
}  // namespace Config
