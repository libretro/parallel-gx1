// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

#include "Common/Align.h"
#include "Common/CommonTypes.h"

#include "Core/PowerPC/MMU.h"

namespace Core
{
class System;
}  // namespace Core

namespace HLE::SystemVABI
{
template <typename T>
concept ArgPointer = std::is_union_v<T> || std::is_class_v<T>;
template <typename T>
concept ArgWord = std::is_pointer_v<T> || (std::is_integral_v<T> && sizeof(T) <= 4);
template <typename T>
concept ArgDoubleWord = std::is_integral_v<T> && sizeof(T) == 8;
template <typename T>
concept ArgReal = std::is_floating_point_v<T>;

// See System V ABI (SVR4) for more details
//  -> 3-18 Parameter Passing
//  -> 3-21 Variable Argument Lists
//
// Source:
// http://refspecs.linux-foundation.org/elf/elfspec_ppc.pdf
class VAList
{
public:
  explicit VAList(u32 stack, u32 gpr = 3, u32 fpr = 1, u32 gpr_max = 10, u32 fpr_max = 8)
      : m_gpr(gpr), m_fpr(fpr), m_gpr_max(gpr_max), m_fpr_max(fpr_max), m_stack(stack)
  {
  }

  // 0 - arg_ARGPOINTER
  template <ArgPointer T>
  T GetArg()
  {
    T obj;
    u32 addr = GetArg<u32>();

    for (size_t i = 0; i < sizeof(T); i += 1, addr += 1)
    {
      reinterpret_cast<u8*>(&obj)[i] = PowerPC::MMU::HostRead<u8>(addr);
    }

    return obj;
  }

  // 1 - arg_WORD
  template <ArgWord T>
  T GetArg()
  {
    static_assert(!std::is_pointer<T>(), "VAList doesn't support pointers");
    u64 value;

    if (m_gpr <= m_gpr_max)
    {
      value = GetGPR(m_gpr);
      m_gpr += 1;
    }
    else
    {
      m_stack = Common::AlignUp(m_stack, 4);
      value = PowerPC::MMU::HostRead<u32>(m_stack);
      m_stack += 4;
    }

    return static_cast<T>(value);
  }

  // 2 - arg_DOUBLEWORD
  template <ArgDoubleWord T>
  T GetArg()
  {
    u64 value;

    if (m_gpr % 2 == 0)
      m_gpr += 1;
    if (m_gpr < m_gpr_max)
    {
      value = static_cast<u64>(GetGPR(m_gpr)) << 32 | GetGPR(m_gpr + 1);
      m_gpr += 2;
    }
    else
    {
      m_stack = Common::AlignUp(m_stack, 8);
      value = PowerPC::MMU::HostRead<u64>(m_stack);
      m_stack += 8;
    }

    return static_cast<T>(value);
  }

  // 3 - arg_ARGREAL
  template <ArgReal T>
  T GetArg()
  {
    double value;

    if (m_fpr <= m_fpr_max)
    {
      value = GetFPR(m_fpr);
      m_fpr += 1;
    }
    else
    {
      m_stack = Common::AlignUp(m_stack, 8);
      value = PowerPC::MMU::HostRead<double>(m_stack);
      m_stack += 8;
    }

    return static_cast<T>(value);
  }

  // Helper
  template <typename T>
  T GetArgT()
  {
    return static_cast<T>(GetArg<T>());
  }

protected:
  u32 m_gpr = 3;
  u32 m_fpr = 1;
  const u32 m_gpr_max = 10;
  const u32 m_fpr_max = 8;
  u32 m_stack;

  // Set by VAListStruct: the registers are read from the va_list's save area, not the CPU
  bool m_from_struct = false;
  bool m_has_fpr_area = false;
  u32 m_address = 0;
  u32 m_reg_save_area = 0;

private:
  u32 GetGPR(u32 gpr) const;
  double GetFPR(u32 fpr) const;
};

// See System V ABI (SVR4) for more details
//  -> 6-6 Required Routines
//  -> 3-21 Variable Argument Lists
//
// Source:
// http://refspecs.linux-foundation.org/elf/elfspec_ppc.pdf
// Adds no members, so it can be held as the VAList it is
class VAListStruct final : public VAList
{
public:
  explicit VAListStruct(u32 address);
};

}  // namespace HLE::SystemVABI
