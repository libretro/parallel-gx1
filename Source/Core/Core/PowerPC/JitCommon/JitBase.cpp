// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitCommon/JitBase.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <utility>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/Thread.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <alloca.h>
#include <sys/mman.h>

#include <cerrno>
#endif

// The BLR optimization is nice, but it means that JITted code can overflow the
// native stack by repeatedly running BL.  (The chance of this happening in any
// retail game is close to 0, but correctness is correctness...) Also, the
// overflow might not happen directly in the JITted code but in a C++ function
// called from it, so we can't just adjust RSP in the case of a fault.
// Instead, we have to have extra stack space preallocated under the fault
// point which allows the code to continue, after wiping the JIT cache so we
// can reset things at a safe point.  Once this condition trips, the
// optimization is permanently disabled, under the assumption this will never
// happen in practice.

// On Unix, we just mark an appropriate region of the stack as PROT_NONE and
// handle it the same way as fastmem faults.  It's safe to take a fault with a
// bad RSP, because on Linux we can use sigaltstack and on OS X we're already
// on a separate thread.

// Windows is... under-documented.
// It already puts guard pages so it can automatically grow the stack and it
// doesn't look like there is a way to hook into a guard page fault and implement
// our own logic.
// But when windows reaches the last guard page, it raises a "Stack Overflow"
// exception which we can hook into, however by default it leaves you with less
// than 4kb of stack. So we use SetThreadStackGuarantee to trigger the Stack
// Overflow early while we still have 256kb of stack remaining.
// After resetting the stack to the top, we call _resetstkoflw() to restore
// the guard page at the 256kb mark.

const std::array<std::pair<bool JitBase::*, const Config::Info<bool>*>, 23> JitBase::JIT_SETTINGS{{
    {&JitBase::bJITOff, &Config::MAIN_DEBUG_JIT_OFF},
    {&JitBase::bJITLoadStoreOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF},
    {&JitBase::bJITLoadStorelXzOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF},
    {&JitBase::bJITLoadStorelwzOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF},
    {&JitBase::bJITLoadStorelbzxOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF},
    {&JitBase::bJITLoadStoreFloatingOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF},
    {&JitBase::bJITLoadStorePairedOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF},
    {&JitBase::bJITFloatingPointOff, &Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF},
    {&JitBase::bJITIntegerOff, &Config::MAIN_DEBUG_JIT_INTEGER_OFF},
    {&JitBase::bJITPairedOff, &Config::MAIN_DEBUG_JIT_PAIRED_OFF},
    {&JitBase::bJITSystemRegistersOff, &Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF},
    {&JitBase::bJITBranchOff, &Config::MAIN_DEBUG_JIT_BRANCH_OFF},
    {&JitBase::bJITRegisterCacheOff, &Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF},
    {&JitBase::m_enable_branch_following, &Config::MAIN_JIT_FOLLOW_BRANCH},
    {&JitBase::m_enable_float_exceptions, &Config::MAIN_FLOAT_EXCEPTIONS},
    {&JitBase::m_enable_div_by_zero_exceptions, &Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS},
    {&JitBase::m_low_dcbz_hack, &Config::MAIN_LOW_DCBZ_HACK},
    {&JitBase::m_fprf, &Config::MAIN_FPRF},
    {&JitBase::m_accurate_nans, &Config::MAIN_ACCURATE_NANS},
    {&JitBase::m_accurate_fmadds, &Config::MAIN_ACCURATE_FMADDS},
    {&JitBase::m_fastmem_enabled, &Config::MAIN_FASTMEM},
    {&JitBase::m_page_table_fastmem_enabled, &Config::MAIN_PAGE_TABLE_FASTMEM},
    {&JitBase::m_accurate_cpu_cache_enabled, &Config::MAIN_ACCURATE_CPU_CACHE},
}};

const u8* JitBase::Dispatch(JitBase& jit)
{
  return jit.GetBlockCache()->Dispatch();
}

void JitTrampoline(JitBase& jit, u32 em_address)
{
  jit.Jit(em_address);
}

JitBase::JitBase(Core::System& system)
    : m_code_buffer(code_buffer_size), m_system(system), m_ppc_state(system.GetPPCState()),
      m_mmu(system.GetMMU()),
      m_ppc_symbol_db(system.GetPPCSymbolDB())
{
  address_set_init(&js.fifoWriteAddresses, 1024);
  address_set_init(&js.pairedQuantizeAddresses, 1024);
  address_set_init(&js.noSpeculativeConstantsAddresses, 1024);
  m_registered_config_callback_id = CPUThreadConfigCallback::AddConfigChangedCallback([this] {
    if (DoesConfigNeedRefresh())
      ClearCache();
  });
  // The JIT is responsible for calling RefreshConfig on Init and ClearCache
}

JitBase::~JitBase()
{
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_registered_config_callback_id);
  address_set_free(&js.fifoWriteAddresses);
  address_set_free(&js.pairedQuantizeAddresses);
  address_set_free(&js.noSpeculativeConstantsAddresses);
}

bool JitBase::DoesConfigNeedRefresh() const
{
  return std::ranges::any_of(JIT_SETTINGS, [this](const auto& pair) {
    return this->*pair.first != Config::Get(*pair.second);
  });
}

void JitBase::RefreshConfig()
{
  const bool wanted_page_table_mappings = WantsPageTableMappings();

  for (const auto& [member, config_info] : JIT_SETTINGS)
    this->*member = Config::Get(*config_info);

  if (m_accurate_cpu_cache_enabled)
  {
    m_fastmem_enabled = false;
    // This hack is unneeded if the data cache is being emulated.
    m_low_dcbz_hack = false;
  }

  analyzer.SetBranchFollowingEnabled(m_enable_branch_following);
  analyzer.SetFloatExceptionsEnabled(m_enable_float_exceptions);
  analyzer.SetDivByZeroExceptionsEnabled(m_enable_div_by_zero_exceptions);

  jo.fastmem = m_fastmem_enabled && jo.fastmem_arena && EMM::IsExceptionHandlerSupported();
  jo.memcheck = m_system.IsMMUMode() || m_system.IsPauseOnPanicMode();
  jo.fp_exceptions = m_enable_float_exceptions;
  jo.div_by_zero_exceptions = m_enable_div_by_zero_exceptions;

  if (wanted_page_table_mappings != WantsPageTableMappings())
  {
    // Mustn't call this if we're still initializing - it'll cause a crash
    if (Core::IsRunning(m_system))
      m_system.GetMMU().PageTableUpdated();
  }
}

bool JitBase::WantsPageTableMappings() const
{
  return jo.fastmem && m_page_table_fastmem_enabled;
}

void JitBase::InitFastmemArena()
{
  auto& memory = m_system.GetMemory();
  jo.fastmem_arena = Config::Get(Config::MAIN_FASTMEM_ARENA) && memory.InitFastmemArena();
}

void JitBase::InitBLROptimization()
{
  m_enable_blr_optimization =
      jo.enableBlocklink && EMM::IsExceptionHandlerSupported();
  m_cleanup_after_stackfault = false;
}

#ifdef __linux__
// Writes a byte in each page of the stack this far below the caller, highest first, so the stack
// grows down over them.
[[gnu::noinline]] static void TouchStackDown(size_t bytes, long page_size)
{
  volatile u8* const below = static_cast<volatile u8*>(alloca(bytes));
  size_t offset = bytes;
  while (offset)
  {
    offset = offset > static_cast<size_t>(page_size) ? offset - page_size : 0;
    below[offset] = 0;
  }
}
#endif

void JitBase::ProtectStack()
{
  if (!m_enable_blr_optimization)
    return;

#ifdef _WIN32
  ULONG reserveSize = SAFE_STACK_SIZE;
  if (!SetThreadStackGuarantee(&reserveSize))
  {
    PanicAlert("Failed to set thread stack guarantee");
    m_enable_blr_optimization = false;
    return;
  }
#else
  auto [stack_addr, stack_size] = Common::GetCurrentThreadStack();

  const uintptr_t stack_base_addr = reinterpret_cast<uintptr_t>(stack_addr);
  const uintptr_t stack_middle_addr = reinterpret_cast<uintptr_t>(&stack_addr);
  if (stack_middle_addr < stack_base_addr || stack_middle_addr >= stack_base_addr + stack_size)
  {
    PanicAlert("Failed to get correct stack base");
    m_enable_blr_optimization = false;
    return;
  }

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
  {
    PanicAlert("Failed to get page size");
    m_enable_blr_optimization = false;
    return;
  }

  uintptr_t stack_guard_addr = Common::AlignUp(stack_base_addr + GUARD_OFFSET, page_size);
#ifdef __linux__
  // A stack that grows on demand (the process's main thread, where a frontend runs the core on
  // one thread) has no pages mapped yet below what it has used, and a guard there can't be
  // protected. The guard goes what the JIT needs below this frame instead, and the pages down to
  // it are touched once so they are mapped; the place is kept for the next runs on this stack.
  if (m_stack_guard_base == stack_base_addr && m_stack_guard_at)
  {
    stack_guard_addr = m_stack_guard_at;
  }
  else
  {
    unsigned char resident;
    if (mincore(reinterpret_cast<void*>(stack_guard_addr), page_size, &resident) != 0 &&
        errno == ENOMEM)
    {
      constexpr size_t NEAR_STACK = 1024 * 1024;
      const uintptr_t near_guard =
          Common::AlignDown(stack_middle_addr - NEAR_STACK - GUARD_SIZE, page_size);
      if (near_guard > stack_guard_addr)
        stack_guard_addr = near_guard;
      TouchStackDown(stack_middle_addr - stack_guard_addr, page_size);
    }
    m_stack_guard_base = stack_base_addr;
    m_stack_guard_at = stack_guard_addr;
  }
#endif
  if (stack_guard_addr >= stack_middle_addr ||
      stack_middle_addr - stack_guard_addr < GUARD_SIZE + MIN_UNSAFE_STACK_SIZE)
  {
    PanicAlert("Stack is too small for BLR optimization (size %" PRIx64 ", base %" PRIx64
               ", current stack "
               "pointer %" PRIx64 ", alignment %" PRIx64 ")",
               (u64)stack_size, (u64)stack_base_addr, (u64)stack_middle_addr, (u64)(u64)page_size);
    m_enable_blr_optimization = false;
    return;
  }

  m_stack_guard = reinterpret_cast<u8*>(stack_guard_addr);
  if (!Common::ReadProtectMemory(m_stack_guard, GUARD_SIZE))
  {
    m_stack_guard = nullptr;
    m_enable_blr_optimization = false;
    return;
  }
#endif
}

void JitBase::UnprotectStack()
{
#ifndef _WIN32
  if (m_stack_guard)
  {
    Common::UnWriteProtectMemory(m_stack_guard, GUARD_SIZE);
    m_stack_guard = nullptr;
  }
#endif
}

bool JitBase::HandleStackFault()
{
  // It's possible the stack fault might have been caused by something other than
  // the BLR optimization. If the fault was triggered from another thread, or
  // when BLR optimization isn't enabled then there is nothing we can do about the fault.
  // Return false so the regular stack overflow handler can trigger (which crashes)
  if (!m_enable_blr_optimization || !Core::IsCPUThread())
    return false;

  WARN_LOG(POWERPC, "BLR cache disabled due to excessive BL in the emulated program.");

  UnprotectStack();
  m_enable_blr_optimization = false;

  // We're going to need to clear the whole cache to get rid of the bad
  // CALLs, but we can't yet.  Fake the downcount so we're forced to the
  // dispatcher (no block linking), and clear the cache so we're sent to
  // Jit. In the case of Windows, we will also need to call _resetstkoflw()
  // to reset the guard page.
  // Yeah, it's kind of gross.
  GetBlockCache()->InvalidateICache(0, 0xffffffff, true);
  m_system.GetCoreTiming().ForceExceptionCheck(0);
  m_cleanup_after_stackfault = true;

  return true;
}

void JitBase::CleanUpAfterStackFault()
{
  if (m_cleanup_after_stackfault)
  {
    ClearCache();
    m_cleanup_after_stackfault = false;
#ifdef _WIN32
    // The stack is in an invalid state with no guard page, reset it.
    _resetstkoflw();
#endif
  }
}

bool JitBase::CanMergeNextInstructions(int count) const
{
  if (m_system.GetCPU().IsStepping() || js.instructionsLeft < count)
    return false;
  return true;
}

bool JitBase::ShouldHandleFPExceptionForInstruction(const PPCAnalyst::CodeOp* op) const
{
  if (jo.fp_exceptions)
    return (op->opinfo->flags & FL_FLOAT_EXCEPTION) != 0;
  else if (jo.div_by_zero_exceptions)
    return (op->opinfo->flags & FL_FLOAT_DIV) != 0;
  else
    return false;
}
