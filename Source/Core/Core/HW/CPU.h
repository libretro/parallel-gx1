// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

#include "Common/Event.h"
#include "Common/Functional.h"

namespace Common
{
class Event;
}
namespace Core
{
class System;
}
namespace PowerPC
{
enum class CPUCore;
}

namespace CPU
{
enum class State
{
  Running = 0,
  Stepping = 2,
  PowerDown = 3
};

class CPUManager
{
public:
  explicit CPUManager(Core::System& system);
  CPUManager(const CPUManager& other) = delete;
  CPUManager(CPUManager&& other) = delete;
  CPUManager& operator=(const CPUManager& other) = delete;
  CPUManager& operator=(CPUManager&& other) = delete;
  ~CPUManager();

  // Init
  void Init(PowerPC::CPUCore cpu_core);

  // Shutdown
  void Shutdown();

  // Starts the CPU
  // To be called by the CPU Thread.
  void Run();

#ifdef __LIBRETRO__
  void RunSingleFrame();
#endif

  // Causes shutdown
  // Once started, State::PowerDown cannot be stopped.
  // Synchronizes with the CPU Thread (waits for CPU::Run to exit).
  void Stop();

  // Reset [NOT IMPLEMENTED]
  void Reset();

  // StepOpcode (Steps one Opcode)
  void StepOpcode(Common::Event* event = nullptr);

  // Enable or Disable Stepping.
  void SetStepping(bool stepping);

  // Breakpoint activation for system threads. Similar to SetStepping(true).
  // NOTE: Unlike SetStepping, this does NOT synchronize with the CPU Thread
  //   which enables it to avoid deadlocks but also makes it less safe so it
  //   should not be used by the Host.
  void Break();

  // This should only be called from the CPU thread
  void Continue();

  // Shorthand for GetState() == State::Stepping.
  // WARNING: State::PowerDown will return false, not just State::Running.
  bool IsStepping() const;

  // Get current CPU Thread State
  State GetState() const;


  // Direct State Access (Raw pointer for embedding into JIT Blocks)
  // Strictly read-only.
  const State* GetStatePtr() const;

  // Locks the CPU Thread (waiting for it to become idle). While this lock is held, the CPU Thread
  // will not perform any action so it is safe to access PowerPC, CoreTiming, etc. without racing
  // the CPU Thread.
  //
  // Cannot be used recursively. Cannot be used by System threads as it will deadlock. It is
  // threadsafe otherwise.
  //
  // Each call to PauseAndLock must be paired with a call to RestoreStateAndUnlock. The return value
  // for PauseAndLock is whether the state was State::Running or not and should be passed as the
  // argument to RestoreStateAndUnlock.
  bool PauseAndLock();
  void RestoreStateAndUnlock(bool unpause_on_unlock);

  // Adds a job to be executed during on the CPU thread. This should be combined with
  // PauseAndLock(), as while the CPU is in the run loop, it won't execute the function.
  void AddCPUThreadJob(Common::MoveOnlyFunction<void()> function);

#ifdef __LIBRETRO__
  bool HasCPURunStateBeenReached() const
  {
    return retro_atomic_load_acquire_int(&m_cpu_run_state_reached) != 0;
  }
#endif
private:
  struct Job
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back to jobs
    Common::MoveOnlyFunction<void()> function;
  };

  void FlushStepSyncEvent();
  void ExecutePendingJobs();
  void StartTimePlayedTimer();
  void RunAdjacentSystems(bool running);
  bool SetStateChecked(State s);
  State LoadState() const;
  // Marks the CPU thread active unless the state it is about to run in was changed or a pause
  // asked for meanwhile; false means it backed off and must look again.
  bool TryEnterActive(State expected);
  void LeaveActive();
  // Waits for the CPU thread to leave the state it was running in.
  void WaitForIdle();
  void ClaimStepping();
  void ReleaseStepping();
  void WakeCPU() { retro_eventcount_notify(&m_cpu_wake); }

  // CPU Thread execution state, a State. Written by the host threads and the CPU thread with
  // seq_cst stores, read by the JIT through GetStatePtr.
  retro_atomic_int_t m_state;

  // Only one SetStepping or PauseAndLock is in progress at a time: they may wait for the CPU
  // thread, and two waiting together would need extra flags to tell their requests apart.
  retro_atomic_int_t m_stepping_claimed;
  retro_eventcount_t m_stepping_released;
  // Only touched by the thread holding the stepping claim.
  bool m_have_fake_cpu_thread = false;

  // The CPU thread is inside RunLoop or SingleStep.
  retro_atomic_int_t m_cpu_thread_active;
  retro_atomic_int_t m_paused_and_locked;
  retro_atomic_int_t m_system_request_stepping;
  // Wakes the CPU thread: state changed, a pause ended, a step was asked for.
  retro_eventcount_t m_cpu_wake;
  // Wakes threads waiting for the CPU thread to go idle.
  retro_eventcount_t m_cpu_idle;

  // A step request: null for none, STEP_NO_EVENT for one nobody waits on, or the Event to set
  // once the step is done. Exchanged whole, so a request and its event never come apart.
  retro_atomic_ptr_t m_step_request;

  mpsc_stack_t m_pending_jobs;
  Common::Event m_time_played_finish_sync;

  Core::System& m_system;
#ifdef __LIBRETRO__
  retro_atomic_int_t m_cpu_run_state_reached;
#endif
};
}  // namespace CPU
