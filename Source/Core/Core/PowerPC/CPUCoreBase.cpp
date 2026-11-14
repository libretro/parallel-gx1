// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/CPUCoreBase.h"

#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif

#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

#ifdef _M_X86_64
#define CPU_CORE_JIT64(call)                                                                       \
  case CPUCoreKind::Jit64:                                                                         \
    return static_cast<Jit64*>(this)->call;
#else
#define CPU_CORE_JIT64(call)
#endif

#ifdef _M_ARM_64
#define CPU_CORE_JITARM64(call)                                                                    \
  case CPUCoreKind::JitArm64:                                                                      \
    return static_cast<JitArm64*>(this)->call;
#else
#define CPU_CORE_JITARM64(call)
#endif

/* The call made on the core this is */
#define CPU_CORE_CALL(call)                                                                        \
  switch (cpu_core_kind)                                                                           \
  {                                                                                                \
  case CPUCoreKind::Interpreter:                                                                   \
    return static_cast<Interpreter*>(this)->call;                                                  \
  case CPUCoreKind::CachedInterpreter:                                                             \
    return static_cast<CachedInterpreter*>(this)->call;                                            \
  case CPUCoreKind::FifoPlayer:                                                                    \
    return static_cast<FifoPlayerCPUCore*>(this)->call;                                            \
    CPU_CORE_JIT64(call)                                                                           \
    CPU_CORE_JITARM64(call)                                                                        \
  default:                                                                                         \
    break;                                                                                         \
  }

void CPUCoreBase::Init()
{
  CPU_CORE_CALL(Init())
}

void CPUCoreBase::Shutdown()
{
  CPU_CORE_CALL(Shutdown())
}

void CPUCoreBase::ClearCache()
{
  CPU_CORE_CALL(ClearCache())
}

void CPUCoreBase::Run()
{
  CPU_CORE_CALL(Run())
}

void CPUCoreBase::SingleStep()
{
  CPU_CORE_CALL(SingleStep())
}

const char* CPUCoreBase::GetName() const
{
  CPUCoreBase* const self = const_cast<CPUCoreBase*>(this);
  switch (cpu_core_kind)
  {
  case CPUCoreKind::Interpreter:
    return static_cast<Interpreter*>(self)->GetName();
  case CPUCoreKind::CachedInterpreter:
    return static_cast<CachedInterpreter*>(self)->GetName();
  case CPUCoreKind::FifoPlayer:
    return static_cast<FifoPlayerCPUCore*>(self)->GetName();
#ifdef _M_X86_64
  case CPUCoreKind::Jit64:
    return static_cast<Jit64*>(self)->GetName();
#endif
#ifdef _M_ARM_64
  case CPUCoreKind::JitArm64:
    return static_cast<JitArm64*>(self)->GetName();
#endif
  default:
    return "";
  }
}
