// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

#include "Common/Functional.h"

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
  // Left the run loop to take the jobs queued for this thread, then running again
  RunJobs = 1,
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

  void RunSingleFrame();

  // Causes shutdown
  // Once started, State::PowerDown cannot be stopped.
  // Synchronizes with the CPU Thread (waits for CPU::Run to exit).
  void Stop();

  // Reset [NOT IMPLEMENTED]
  void Reset();

  // Enable or Disable Stepping.
  void SetStepping(bool stepping);

  // Breakpoint activation for system threads. Similar to SetStepping(true).
  // NOTE: Unlike SetStepping, this does NOT synchronize with the CPU Thread
  //   which enables it to avoid deadlocks but also makes it less safe so it
  //   should not be used by the Host.
  void Break();

  // Shorthand for GetState() == State::Stepping.
  // WARNING: State::PowerDown will return false, not just State::Running.
  bool IsStepping() const;

  // Get current CPU Thread State
  State GetState() const;


  // Direct State Access (Raw pointer for embedding into JIT Blocks)
  // Strictly read-only.
  const State* GetStatePtr() const;

  // Any thread: queues a job for the CPU thread, which leaves the run loop for it at the end of
  // the timing slice, or takes it parked or stepping; done, when given, is set once it ran or was
  // dropped at shutdown.
  void AddCPUThreadJob(Common::MoveOnlyFunction<void()> function,
                       retro_atomic_int_t* done = nullptr);
  // Until the job given done has been taken.
  void WaitForJob(retro_atomic_int_t* done);
  bool HasPendingJobs();

  bool HasCPURunStateBeenReached() const
  {
    return retro_atomic_load_acquire_int(&m_cpu_run_state_reached) != 0;
  }
private:
  struct Job
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back to jobs
    Common::MoveOnlyFunction<void()> function;
    retro_atomic_int_t* done;
  };

  void FinishJob(Job* job);
  void DropPendingJobs();

  void ExecutePendingJobs();
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

  // Only one SetStepping is in progress at a time: it may wait for the CPU thread, and two
  // waiting together would need extra flags to tell their requests apart.
  retro_atomic_int_t m_stepping_claimed;
  retro_eventcount_t m_stepping_released;

  // The CPU thread is inside RunLoop or SingleStep.
  retro_atomic_int_t m_cpu_thread_active;
  // Wakes the CPU thread: state changed, or a job was queued.
  retro_eventcount_t m_cpu_wake;
  // Wakes threads waiting for a job they queued.
  retro_eventcount_t m_jobs_done;
  // Wakes threads waiting for the CPU thread to go idle.
  retro_eventcount_t m_cpu_idle;


  mpsc_stack_t m_pending_jobs;

  Core::System& m_system;
  retro_atomic_int_t m_cpu_run_state_reached;
};
}  // namespace CPU
