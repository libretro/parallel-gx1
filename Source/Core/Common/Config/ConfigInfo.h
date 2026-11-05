// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Config/Enums.h"
#include "Common/Quiesce.h"
#include "Common/TypeUtils.h"

namespace Config
{
struct Location
{
  System system{};
  std::string section;
  std::string key;

  bool operator==(const Location& other) const;
  bool operator<(const Location& other) const;
};

template <typename T>
struct CachedValue
{
  T value;
  u64 config_version;
};

namespace detail
{
// Guards the cached values of Infos too wide for one atomic word.
quiesce_t* InfoQuiesce();

// Small trivially copyable values share one 64-bit word with their config version, so a
// read is one load and an update one CAS.
template <typename T>
constexpr bool PACKED_CACHE = std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(u32);

// node first: the free callback gets the node back and casts it to the whole.
template <typename T>
struct CacheRef
{
  quiesce_node_t node;
  CachedValue<T> cached;
};
}  // namespace detail

template <typename T>
class Info
{
public:
  constexpr Info(Location location, T default_value)
      : m_location{std::move(location)}, m_default_value{std::move(default_value)}
  {
  }

  Info(const Info<T>& other)
      : m_location{other.m_location}, m_default_value{other.m_default_value}
  {
    TryToSetCachedValue(other.GetCachedValue());
  }

  // Make it easy to convert Info<Enum> into Info<UnderlyingType<Enum>>
  // so that enum settings can still easily work with code that doesn't care about the enum values.
  template <Common::TypedEnum<T> Enum>
  Info(const Info<Enum>& other)
      : m_location{other.GetLocation()}, m_default_value{static_cast<T>(other.GetDefaultValue())}
  {
    TryToSetCachedValue(other.template GetCachedValueCasted<T>());
  }

  ~Info()
  {
    if constexpr (!detail::PACKED_CACHE<T>)
      delete static_cast<detail::CacheRef<T>*>(retro_atomic_load_acquire_ptr(&m_cache));
  }

  // Assignments after construction would require more locking to be thread safe.
  // It seems unnecessary to have this functionality anyways.
  Info& operator=(const Info&) = delete;
  Info& operator=(Info&&) = delete;
  // Moves are also unnecessary and would be thread unsafe without additional locking.
  Info(Info&&) = delete;

  constexpr const Location& GetLocation() const { return m_location; }
  constexpr const T& GetDefaultValue() const { return m_default_value; }

  // config_version 0 means nothing is cached yet.
  CachedValue<T> GetCachedValue() const
  {
    if constexpr (detail::PACKED_CACHE<T>)
    {
      const u64 word = static_cast<u64>(retro_atomic_load_acquire_64(&m_cache));
      CachedValue<T> result{m_default_value, word >> 32};
      if (result.config_version)
      {
        const u32 bits = static_cast<u32>(word);
        std::memcpy(&result.value, &bits, sizeof(T));
      }
      return result;
    }
    else
    {
      quiesce_t* const q = detail::InfoQuiesce();
      quiesce_enter(q);
      const auto* ref =
          static_cast<const detail::CacheRef<T>*>(retro_atomic_load_acquire_ptr(&m_cache));
      CachedValue<T> result = ref ? ref->cached : CachedValue<T>{m_default_value, 0};
      quiesce_exit(q);
      return result;
    }
  }

  template <typename U>
  CachedValue<U> GetCachedValueCasted() const
  {
    const CachedValue<T> v = GetCachedValue();
    return {static_cast<U>(v.value), v.config_version};
  }

  // Only updates if the provided config_version is newer.
  void TryToSetCachedValue(CachedValue<T> new_value) const
  {
    if constexpr (detail::PACKED_CACHE<T>)
    {
      u32 bits = 0;
      std::memcpy(&bits, &new_value.value, sizeof(T));
      const u64 word = (static_cast<u64>(static_cast<u32>(new_value.config_version)) << 32) | bits;
      int64_t cur = retro_atomic_load_acquire_64(&m_cache);
      while ((static_cast<u64>(cur) >> 32) < (word >> 32))
      {
        if (retro_atomic_cas_64(&m_cache, cur, static_cast<int64_t>(word)))
          return;
        cur = retro_atomic_load_acquire_64(&m_cache);
      }
    }
    else
    {
      auto* ref = new detail::CacheRef<T>{{}, std::move(new_value)};
      void* cur = retro_atomic_load_acquire_ptr(&m_cache);
      for (;;)
      {
        const auto* cur_ref = static_cast<const detail::CacheRef<T>*>(cur);
        if (cur_ref && cur_ref->cached.config_version >= ref->cached.config_version)
        {
          delete ref;
          return;
        }
        if (retro_atomic_cas_ptr(&m_cache, cur, ref))
          break;
        cur = retro_atomic_load_acquire_ptr(&m_cache);
      }
      if (cur)
      {
        quiesce_retire(detail::InfoQuiesce(), &static_cast<detail::CacheRef<T>*>(cur)->node,
                       [](quiesce_node_t* n) { delete reinterpret_cast<detail::CacheRef<T>*>(n); });
      }
    }
  }

private:
  Location m_location;
  T m_default_value;

  // Packed: config version in the high half, the value's bits in the low half.
  // Otherwise: a detail::CacheRef<T>* or null.
  mutable std::conditional_t<detail::PACKED_CACHE<T>, retro_atomic_64_t, retro_atomic_ptr_t>
      m_cache{};
};
}  // namespace Config
