// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <array>
#include <cinttypes>
#include <string>

#include <cstdio>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// Determines whether or not the given instruction is one where its execution
// validity is determined by whether or not HID2's LSQE bit is set.
// In other words, if the instruction is psq_l, psq_lu, psq_st, or psq_stu
bool IsPairedSingleQuantizedNonIndexedInstruction(UGeckoInstruction inst)
{
  const u32 opcode = inst.OPCD;
  return opcode == 0x38 || opcode == 0x39 || opcode == 0x3C || opcode == 0x3D;
}

bool IsPairedSingleInstruction(UGeckoInstruction inst)
{
  return inst.OPCD == 4 || IsPairedSingleQuantizedNonIndexedInstruction(inst);
}
}  // namespace

// Checks if a given instruction would be illegal to execute if it's a paired single instruction.
//
// Paired single instructions are illegal to execute if HID2.PSE is not set.
// It's also illegal to execute psq_l, psq_lu, psq_st, and psq_stu if HID2.PSE is enabled,
// but HID2.LSQE is not set.
bool Interpreter::IsInvalidPairedSingleExecution(UGeckoInstruction inst)
{
  if (!HID2(m_ppc_state).PSE && IsPairedSingleInstruction(inst))
    return true;

  return HID2(m_ppc_state).PSE && !HID2(m_ppc_state).LSQE &&
         IsPairedSingleQuantizedNonIndexedInstruction(inst);
}

void Interpreter::UpdatePC()
{
  m_last_pc = m_ppc_state.pc;
  m_ppc_state.pc = m_ppc_state.npc;
}

Interpreter::Interpreter(Core::System& system, PowerPC::PowerPCState& ppc_state, PowerPC::MMU& mmu,
                         PPCSymbolDB& ppc_symbol_db)
    : m_system(system), m_ppc_state(ppc_state), m_mmu(mmu), m_ppc_symbol_db(ppc_symbol_db)
{
}

Interpreter::~Interpreter() = default;

void Interpreter::Init()
{
  m_end_block = false;
}

void Interpreter::Shutdown()
{
}

bool Interpreter::HandleFunctionHooking(u32 address)
{
  const auto result =
      HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::Interpreter);
  if (!result)
    return false;

  HLEFunction(*this, result.hook_index);

  return result.type != HLE::HookType::Start;
}

int Interpreter::SingleStepInner()
{
  if (HandleFunctionHooking(m_ppc_state.pc))
  {
    UpdatePC();
    // TODO: Does it make sense to use m_prev_inst here?
    // It seems like we should use the num_cycles for the instruction at PC instead
    // (m_prev_inst has not yet been updated)
    return PPCTables::GetOpInfo(m_prev_inst, m_ppc_state.pc)->num_cycles;
  }

  m_ppc_state.npc = m_ppc_state.pc + sizeof(UGeckoInstruction);
  m_prev_inst.hex = m_mmu.Read_Opcode(m_ppc_state.pc);

  const GekkoOPInfo* opinfo = PPCTables::GetOpInfo(m_prev_inst, m_ppc_state.pc);

  if (m_prev_inst.hex != 0)
  {
    if (IsInvalidPairedSingleExecution(m_prev_inst))
    {
      GenerateProgramException(m_ppc_state, ProgramExceptionCause::IllegalInstruction);
      CheckExceptions();
    }
    else if (m_ppc_state.msr.FP)
    {
      RunInterpreterOp(*this, m_prev_inst);
      if ((m_ppc_state.Exceptions & EXCEPTION_DSI) != 0)
      {
        CheckExceptions();
      }
    }
    else
    {
      // check if we have to generate a FPU unavailable exception or a program exception.
      if ((opinfo->flags & FL_USE_FPU) != 0)
      {
        m_ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
        CheckExceptions();
      }
      else
      {
        RunInterpreterOp(*this, m_prev_inst);
        if ((m_ppc_state.Exceptions & EXCEPTION_DSI) != 0)
        {
          CheckExceptions();
        }
      }
    }
  }
  else
  {
    // Memory exception on instruction fetch
    CheckExceptions();
  }

  UpdatePC();

  PowerPC::UpdatePerformanceMonitor(opinfo->num_cycles, (opinfo->flags & FL_LOADSTORE) != 0,
                                    (opinfo->flags & FL_USE_FPU) != 0, m_ppc_state);
  return opinfo->num_cycles;
}

void Interpreter::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& core_timing_globals = core_timing.GetGlobals();

  // Declare start of new slice
  core_timing.Advance();

  SingleStepInner();

  // The interpreter ignores instruction timing information outside the 'fast runloop'.
  core_timing_globals.slice_length = 1;
  m_ppc_state.downcount = 0;

  if (m_ppc_state.Exceptions != 0)
  {
    m_system.GetPowerPC().CheckExceptions();
    m_ppc_state.pc = m_ppc_state.npc;
  }
}

// FastRun - inspired by GCemu (to imitate the JIT so that they can be compared).
void Interpreter::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  while (cpu.GetState() == CPU::State::Running)
  {
    // CoreTiming Advance() ends the previous slice and declares the start of the next
    // one so it must always be called at the start. At boot, we are in slice -1 and must
    // advance into slice 0 to get a correct slice length before executing any cycles.
    core_timing.Advance();

    while (m_ppc_state.downcount > 0)
    {
      int cycles = 0;
      m_end_block = false;
      while (!m_end_block)
        cycles += SingleStepInner();
      m_ppc_state.downcount -= cycles;
    }
  }
}

void Interpreter::unknown_instruction(Interpreter& interpreter, UGeckoInstruction inst)
{
  ASSERT(Core::IsCPUThread());
  auto& system = interpreter.m_system;
  Core::CPUThreadGuard guard(system);

  const u32 last_pc = interpreter.m_last_pc;
  const u32 opcode = PowerPC::MMU::HostRead<u32>(guard, last_pc);
  const std::string disasm = Common::GekkoDisassembler::Disassemble(opcode, last_pc);
  NOTICE_LOG(POWERPC, "Last PC = %08x : %s", last_pc, disasm.c_str());
  Dolphin_Debugger::PrintCallstack(guard, Common::Log::LogType::POWERPC,
                                   Common::Log::LogLevel::LNOTICE);

  const auto& ppc_state = interpreter.m_ppc_state;
  NOTICE_LOG(POWERPC,
             "\nIntCPU: Unknown instruction %08x at PC = %08x  last_PC = %08x  LR = %08x\n",
             inst.hex, ppc_state.pc, last_pc, LR(ppc_state));
  for (int i = 0; i < 32; i += 4)
  {
    NOTICE_LOG(POWERPC, "r%d: %#010x r%d: %#010x r%d: %#010x r%d: %#010x", i, ppc_state.gpr[i],
               i + 1, ppc_state.gpr[i + 1], i + 2, ppc_state.gpr[i + 2], i + 3,
               ppc_state.gpr[i + 3]);
  }
  ASSERT_MSG(POWERPC, 0,
             "\nIntCPU: Unknown instruction {:08x} at PC = {:08x}  last_PC = {:08x}  LR = {:08x}\n",
             inst.hex, ppc_state.pc, last_pc, LR(ppc_state));
  if (system.IsPauseOnPanicMode())
    system.GetCPU().Break();
}

void Interpreter::ClearCache()
{
  // Do nothing.
}

void Interpreter::CheckExceptions()
{
  m_system.GetPowerPC().CheckExceptions();
  m_end_block = true;
}

const char* Interpreter::GetName() const
{
#ifdef _ARCH_64
  return "Interpreter64";
#else
  return "Interpreter32";
#endif
}
