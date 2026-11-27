// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Extremely simple serialization framework.

// (mis)-features:
// + Super fast
// + Very simple
// + Same code is used for serialization and deserializaition (in most cases)
// - Zero backwards/forwards compatibility
// - Serialization code for anything complex has to be manually written.

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Flag.h"
#include "Common/Inline.h"

#include <retro_atomic.h>

// Wrapper class
class PointerWrap
{
public:
  enum class Mode
  {
    Read,
    Write,
    Measure,
    Verify,
  };

private:
  u8** m_ptr_current;
  u8* m_ptr_end;
  Mode m_mode;

public:
  PointerWrap(u8** ptr, size_t size, Mode mode)
      : m_ptr_current(ptr), m_ptr_end(*ptr + size), m_mode(mode)
  {
  }

  void SetMeasureMode() { m_mode = Mode::Measure; }
  void SetVerifyMode() { m_mode = Mode::Verify; }
  bool IsReadMode() const { return m_mode == Mode::Read; }
  bool IsWriteMode() const { return m_mode == Mode::Write; }
  bool IsMeasureMode() const { return m_mode == Mode::Measure; }
  bool IsVerifyMode() const { return m_mode == Mode::Verify; }

  // A terminated string, kept as its u32 length and its bytes as a std::string is. Read mode
  // keeps what fits in the buffer of size bytes, terminated, and passes over the rest.
  void DoCString(char* buffer, u32 size)
  {
    u32 length = IsReadMode() ? 0 : static_cast<u32>(std::strlen(buffer));
    Do(length);
    u8* const bytes = DoSkip(length);
    if (!bytes || IsMeasureMode())
    {
      if (IsReadMode())
        buffer[0] = '\0';
      return;
    }

    switch (m_mode)
    {
    case Mode::Read:
    {
      const u32 kept = length < size ? length : size - 1;
      std::memcpy(buffer, bytes, kept);
      buffer[kept] = '\0';
      break;
    }
    case Mode::Write:
      std::memcpy(bytes, buffer, length);
      break;
    case Mode::Verify:
      DEBUG_ASSERT_MSG(COMMON, !std::memcmp(buffer, bytes, length),
                       "Savestate verification failure: string %s.\n", buffer);
      break;
    case Mode::Measure:
      break;
    }
  }

  template <typename V, auto last_member, typename = decltype(last_member)>
  void DoArray(Common::EnumMap<V, last_member>& x)
  {
    DoArray(x.data(), static_cast<u32>(x.size()));
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    DoVoid(x, count * sizeof(T));
  }

  template <typename T>
  requires(!std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      Do(x[i]);
  }

  template <typename T, std::size_t N>
  void DoArray(T (&arr)[N])
  {
    DoArray(arr, static_cast<u32>(N));
  }

  // The caller is required to inspect the mode of this PointerWrap
  // and deal with the pointer returned from this function themself.
  [[nodiscard]] u8* DoExternal(u32& count)
  {
    Do(count);
    u8* current = *m_ptr_current;
    *m_ptr_current += count;
    if (!IsMeasureMode() && *m_ptr_current > m_ptr_end)
    {
      // trying to read/write past the end of the buffer, prevent this
      SetMeasureMode();
    }
    return current;
  }

  // The next size bytes, passed over: the caller reads them in read mode and fills them in write
  // mode. nullptr past the end of the buffer.
  [[nodiscard]] u8* DoSkip(u32 size)
  {
    u8* const current = *m_ptr_current;
    if (!IsMeasureMode() && size > static_cast<size_t>(m_ptr_end - current))
    {
      SetMeasureMode();
      return nullptr;
    }
    *m_ptr_current += size;
    return current;
  }

  void Do(Common::Flag& flag)
  {
    bool s = flag.IsSet();
    Do(s);
    if (IsReadMode())
      flag.Set(s);
  }

  /* An atomic word goes into the state as its plain value. */
  void Do(retro_atomic_int_t& atomic)
  {
    s32 temp = static_cast<s32>(retro_atomic_load_relaxed_int(&atomic));
    Do(temp);
    if (IsReadMode())
      retro_atomic_store_relaxed_int(&atomic, static_cast<int>(temp));
  }

  void Do(retro_atomic_64_t& atomic)
  {
    s64 temp = static_cast<s64>(retro_atomic_load_relaxed_64(&atomic));
    Do(temp);
    if (IsReadMode())
      retro_atomic_store_relaxed_64(&atomic, temp);
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
  void Do(T& x)
  {
    // Note:
    // Usually we can just use x = **ptr, etc.  However, this doesn't work
    // for unions containing BitFields (long story, stupid language rules)
    // or arrays.  This will get optimized anyway.
    DoVoid((void*)&x, sizeof(x));
  }

  void Do(bool& x)
  {
    // bool's size can vary depending on platform, which can
    // cause breakages. This treats all bools as if they were
    // 8 bits in size.
    u8 stable = static_cast<u8>(x);

    Do(stable);

    if (IsReadMode())
      x = stable != 0;
  }

  template <typename T>
  void DoPointer(T*& x, T* const base)
  {
    // pointers can be more than 2^31 apart, but you're using this function wrong if you need that
    // much range
    ptrdiff_t offset = x - base;
    Do(offset);
    if (IsReadMode())
    {
      x = base + offset;
    }
  }

  void DoPointer(void*& x, void* const base)
  {
    auto* ptr = static_cast<u8*>(x);
    DoPointer(ptr, static_cast<u8*>(base));
    x = ptr;
  }

  void DoMarker(const char* prevName, u32 arbitraryNumber = 0x42)
  {
    u32 cookie = arbitraryNumber;
    Do(cookie);

    if (IsReadMode() && cookie != arbitraryNumber)
    {
      PanicAlert("Error: After \"%s\", found %u (%#x) instead of save marker %u (%#x). Aborting "
                 "savestate load...",
                 prevName, cookie, cookie, arbitraryNumber, arbitraryNumber);
      SetMeasureMode();
    }
  }

  template <typename T, typename Functor>
  void DoEachElement(T& container, Functor member)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);

    for (auto& elem : container)
      member(*this, elem);
  }

private:
  DOLPHIN_FORCE_INLINE void DoVoid(void* data, u32 size)
  {
    if (!IsMeasureMode() && (*m_ptr_current + size) > m_ptr_end)
    {
      // trying to read/write past the end of the buffer, prevent this
      SetMeasureMode();
    }

    switch (m_mode)
    {
    case Mode::Read:
      memcpy(data, *m_ptr_current, size);
      break;

    case Mode::Write:
      memcpy(*m_ptr_current, data, size);
      break;

    case Mode::Measure:
      break;

    case Mode::Verify:
      DEBUG_ASSERT_MSG(COMMON, !memcmp(data, *m_ptr_current, size),
                       "Savestate verification failure: buf %p != %p (size %u).\n",
                       static_cast<const void*>(data), static_cast<const void*>(*m_ptr_current),
                       static_cast<unsigned>(size));
      break;
    }

    *m_ptr_current += size;
  }
};
