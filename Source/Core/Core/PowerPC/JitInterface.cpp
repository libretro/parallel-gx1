// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitInterface.h"

#include <string>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

#include "Core/Core.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif

#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

JitInterface::JitInterface(Core::System& system) : m_system(system)
{
}

JitInterface::~JitInterface() = default;

void JitInterface::DoState(PointerWrap& p)
{
  if (m_jit && p.IsReadMode())
    m_jit->ClearCache();
}

CPUCoreBase* JitInterface::InitJitCore(PowerPC::CPUCore core)
{
  switch (core)
  {
#ifdef _M_X86_64
  case PowerPC::CPUCore::JIT64:
    m_jit = std::make_unique<Jit64>(m_system);
    break;
#endif
#ifdef _M_ARM_64
  case PowerPC::CPUCore::JITARM64:
    m_jit = std::make_unique<JitArm64>(m_system);
    break;
#endif
  case PowerPC::CPUCore::CachedInterpreter:
    m_jit = std::make_unique<CachedInterpreter>(m_system);
    break;

  default:
    // Under this case the caller overrides the CPU core to the default and logs that
    // it performed the override.
    m_jit.reset();
    return nullptr;
  }
  m_jit->Init();
  return m_jit.get();
}

CPUCoreBase* JitInterface::GetCore() const
{
  return m_jit.get();
}

#ifndef _ARCH_32
bool JitInterface::WantsPageTableMappings() const
{
  if (!m_jit)
    return false;

  return m_jit->WantsPageTableMappings();
}
#endif

void JitInterface::UpdateMembase()
{
  if (!m_jit)
    return;

  auto& ppc_state = m_system.GetPPCState();
  auto& memory = m_system.GetMemory();
#ifdef _M_ARM_64
  // JitArm64 is currently using the no fastmem arena code path even when only fastmem is off.
  const bool fastmem_arena = m_jit->jo.fastmem;
#else
  const bool fastmem_arena = m_jit->jo.fastmem_arena;
#endif
  if (ppc_state.msr.DR)
  {
    ppc_state.mem_ptr =
        fastmem_arena ? memory.GetLogicalBase() : memory.GetLogicalPageMappingsBase();
  }
  else
  {
    ppc_state.mem_ptr =
        fastmem_arena ? memory.GetPhysicalBase() : memory.GetPhysicalPageMappingsBase();
  }
}

bool JitInterface::HandleFault(uintptr_t access_address, SContext* ctx)
{
  // Prevent nullptr dereference on a crash with no JIT present
  if (!m_jit)
  {
    return false;
  }

  return m_jit->HandleFault(access_address, ctx);
}

bool JitInterface::HandleStackFault()
{
  if (!m_jit)
  {
    return false;
  }

  return m_jit->HandleStackFault();
}

void JitInterface::ClearCache(const Core::CPUThreadGuard&)
{
  if (m_jit)
    m_jit->ClearCache();
}

void JitInterface::ClearSafe()
{
  if (m_jit)
    m_jit->GetBlockCache()->Clear();
}

void JitInterface::InvalidateICache(u32 address, u32 size, bool forced)
{
  if (m_jit)
    m_jit->GetBlockCache()->InvalidateICache(address, size, forced);
}

void JitInterface::InvalidateICacheLine(u32 address)
{
  if (m_jit)
    m_jit->GetBlockCache()->InvalidateICacheLine(address);
}

void JitInterface::InvalidateICacheLines(u32 address, u32 count)
{
  // This corresponds to a PPC code loop that:
  // - calls some form of dcb* instruction on 'address'
  // - increments 'address' by the size of a cache line (0x20 bytes)
  // - decrements 'count' by 1
  // - jumps back to the dcb* instruction if 'count' != 0
  // with an extra optimization for the case of a single cache line invalidation
  if (count == 1)
    InvalidateICacheLine(address);
  else if (count == 0 || count >= static_cast<u32>(0x1'0000'0000 / 32))
    InvalidateICache(address & ~0x1f, 0xffffffff, false);
  else
    InvalidateICache(address & ~0x1f, 32 * count, false);
}

void JitInterface::InvalidateICacheLineFromJIT(JitInterface& jit_interface, u32 address)
{
  jit_interface.InvalidateICacheLine(address);
}

void JitInterface::InvalidateICacheLinesFromJIT(JitInterface& jit_interface, u32 address, u32 count)
{
  jit_interface.InvalidateICacheLines(address, count);
}

void JitInterface::CompileExceptionCheck(ExceptionType type)
{
  if (!m_jit)
    return;

  address_set_t* exception_addresses = nullptr;

  switch (type)
  {
  case ExceptionType::FIFOWrite:
    exception_addresses = &m_jit->js.fifoWriteAddresses;
    break;
  case ExceptionType::PairedQuantize:
    exception_addresses = &m_jit->js.pairedQuantizeAddresses;
    break;
  case ExceptionType::SpeculativeConstants:
    exception_addresses = &m_jit->js.noSpeculativeConstantsAddresses;
    break;
  }

  auto& ppc_state = m_system.GetPPCState();
  if (ppc_state.pc != 0 && !address_set_contains(exception_addresses, ppc_state.pc))
  {
    if (type == ExceptionType::FIFOWrite)
    {
      ASSERT(Core::IsCPUThread());
      Core::CPUThreadGuard guard(m_system);

      // Check in case the code has been replaced since: do we need to do this?
      const OpType optype =
          PPCTables::GetOpInfo(PowerPC::MMU::HostRead<u32>(guard, ppc_state.pc), ppc_state.pc)
              ->type;
      if (optype != OpType::Store && optype != OpType::StoreFP && optype != OpType::StorePS)
        return;
    }
    address_set_insert(exception_addresses, ppc_state.pc);

    // Invalidate the JIT block so that it gets recompiled with the external exception check
    // included.
    m_jit->GetBlockCache()->InvalidateICache(ppc_state.pc, 4, true);
  }
}

void JitInterface::CompileExceptionCheckFromJIT(JitInterface& jit_interface, ExceptionType type)
{
  jit_interface.CompileExceptionCheck(type);
}

void JitInterface::Shutdown()
{
  if (m_jit)
  {
    m_jit->Shutdown();
    m_jit.reset();
  }
}
