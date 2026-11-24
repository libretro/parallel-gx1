// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
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
bool TryParse(const std::string& str_value, T* out)
{
  T value;
  if (!::TryParse(str_value, &value))
    return false;
  *out = value;
  return true;
}

template <Common::Enum T>
bool TryParse(const std::string& str_value, T* out)
{
  std::underlying_type_t<T> value;
  if (!TryParse<std::underlying_type_t<T>>(str_value, &value))
    return false;
  *out = static_cast<T>(value);
  return true;
}

template <>
inline bool TryParse(const std::string& str_value, std::string* out)
{
  *out = str_value;
  return true;
}
}  // namespace detail

template <typename T>
class Info;

class Layer;
using LayerMap = std::map<Location, std::optional<std::string>>;

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

class Section
{
public:
  using iterator = LayerMap::iterator;
  Section(iterator begin_, iterator end_) : m_begin(begin_), m_end(end_) {}
  iterator begin() const { return m_begin; }
  iterator end() const { return m_end; }

private:
  iterator m_begin;
  iterator m_end;
};

class ConstSection
{
public:
  using iterator = LayerMap::const_iterator;
  ConstSection(iterator begin_, iterator end_) : m_begin(begin_), m_end(end_) {}
  iterator begin() const { return m_begin; }
  iterator end() const { return m_end; }

private:
  iterator m_begin;
  iterator m_end;
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
    const auto iter = m_map.find(location);
    if (iter == m_map.end() || !iter->second.has_value())
      return false;
    return detail::TryParse<T>(*iter->second, out);
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
    return Set(location, ValueToString(value));
  }

  bool Set(const Location& location, std::string new_value)
  {
    const auto iter = m_map.find(location);
    if (iter != m_map.end() && iter->second == new_value)
      return false;
    m_is_dirty = true;
    m_map.insert_or_assign(location, std::move(new_value));
    return true;
  }

  void MarkAsDirty() { m_is_dirty = true; }

  Section GetSection(System system, const std::string& section);
  ConstSection GetSection(System system, const std::string& section) const;

  // Explicit load and save of layers
  void Load();
  void Save();

  LayerType GetLayer() const;
  const LayerMap& GetLayerMap() const;

private:
  bool m_is_dirty = false;
  LayerMap m_map;
  const LayerType m_layer;
  ConfigLayerLoader* m_loader = nullptr;  // owned
};
}  // namespace Config
