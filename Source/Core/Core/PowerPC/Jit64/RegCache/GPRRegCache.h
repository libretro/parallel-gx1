// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"

class Jit64;

class GPRRegCache final : public RegCache
{
public:
  explicit GPRRegCache(Jit64& jit);

  bool IsImm(preg_t preg) const;
  u32 Imm32(preg_t preg) const;
  s32 SImm32(preg_t preg) const;

  void SetImmediate32(preg_t preg, u32 imm_value, bool dirty = true);

protected:
  // RegCache calls these as the GPR or FPU cache it is
  friend class RegCache;

  Gen::OpArg R(preg_t preg) const;
  Gen::OpArg GetDefaultLocation(preg_t preg) const;
  void StoreRegister(preg_t preg, const Gen::OpArg& new_loc,
                     IgnoreDiscardedRegisters ignore_discarded_registers);
  void LoadRegister(preg_t preg, Gen::X64Reg new_loc);
  void DiscardImm(preg_t preg);
  const Gen::X64Reg* GetAllocationOrder(size_t* count) const;
  BitSet32 GetRegUtilization() const;
  BitSet32 CountRegsIn(preg_t preg, u32 lookahead) const;
};
