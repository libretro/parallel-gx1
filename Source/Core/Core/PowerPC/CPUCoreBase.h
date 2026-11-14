// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// Which CPU core it is: the calls below branch on it to that core's own function
enum class CPUCoreKind : u8
{
  Interpreter,
  CachedInterpreter,
  Jit64,
  JitArm64,
  FifoPlayer,
};

class CPUCoreBase
{
public:
  void Init();
  void Shutdown();
  void ClearCache();
  void Run();
  void SingleStep();
  const char* GetName() const;

  const CPUCoreKind cpu_core_kind;

protected:
  explicit CPUCoreBase(CPUCoreKind kind) : cpu_core_kind(kind) {}
  // destroyed as the core it is
  ~CPUCoreBase() = default;
};
