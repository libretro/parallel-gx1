// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/CPU.h"


#include "AudioCommon/AudioCommon.h"
#include "Common/Event.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/TimePlayed.h"
#include "VideoCommon/Fifo.h"
#ifdef __LIBRETRO__
#include "VideoCommon/AsyncRequests.h"
#endif

namespace CPU
{
// Stands in for the Event of a step request that has none; never dereferenced.
static Common::Event* const STEP_NO_EVENT = reinterpret_cast<Common::Event*>(alignof(void*));

CPUManager::CPUManager(Core::System& system) : m_system(system)
{
  retro_atomic_int_init(&m_state, static_cast<int>(State::PowerDown));
  retro_atomic_int_init(&m_stepping_claimed, 0);
  retro_atomic_int_init(&m_cpu_thread_active, 0);
  retro_atomic_int_init(&m_paused_and_locked, 0);
  retro_atomic_int_init(&m_system_request_stepping, 0);
  retro_atomic_ptr_init(&m_step_request, nullptr);
  mpsc_stack_init(&m_pending_jobs);
  retro_eventcount_init(&m_stepping_released);
  retro_eventcount_init(&m_cpu_wake);
  retro_eventcount_init(&m_cpu_idle);
#ifdef __LIBRETRO__
  retro_atomic_int_init(&m_cpu_run_state_reached, 0);
#endif
}

CPUManager::~CPUManager()
{
  for (mpsc_stack_node_t* node = mpsc_stack_drain(&m_pending_jobs); node;)
  {
    Job* job = reinterpret_cast<Job*>(node);
    node = node->next;
    delete job;
  }
  retro_eventcount_free(&m_cpu_idle);
  retro_eventcount_free(&m_cpu_wake);
  retro_eventcount_free(&m_stepping_released);
}

State CPUManager::LoadState() const
{
  return static_cast<State>(retro_atomic_load_seq_cst_int(&m_state));
}

void CPUManager::Init(PowerPC::CPUCore cpu_core)
{
  m_system.GetPowerPC().Init(cpu_core);
  retro_atomic_store_release_int(&m_state, static_cast<int>(State::Stepping));
}

void CPUManager::Shutdown()
{
  Stop();
#ifdef __LIBRETRO__
  retro_atomic_store_release_int(&m_cpu_run_state_reached, 0);
#endif
  m_system.GetPowerPC().Shutdown();
}

void CPUManager::FlushStepSyncEvent()
{
  auto* event = static_cast<Common::Event*>(retro_atomic_exchange_ptr(&m_step_request, nullptr));
  if (event && event != STEP_NO_EVENT)
    event->Set();
}

void CPUManager::ExecutePendingJobs()
{
  // The stack hands them back newest first; run them in the order they were added.
  mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_pending_jobs));
  while (node)
  {
    Job* job = reinterpret_cast<Job*>(node);
    node = node->next;
    job->function();
    delete job;
  }
}

bool CPUManager::TryEnterActive(State expected)
{
  // Dekker with the pausing side, which stores its request and then reads this flag: either
  // the request is seen here, or the flag is seen there and the pauser waits for it to drop.
  retro_atomic_fetch_add_seq_cst_int(&m_cpu_thread_active, 1);
  retro_atomic_thread_fence_seq_cst();
  if (!retro_atomic_load_seq_cst_int(&m_paused_and_locked) && LoadState() == expected)
    return true;
  LeaveActive();
  return false;
}

void CPUManager::LeaveActive()
{
  retro_atomic_fetch_add_seq_cst_int(&m_cpu_thread_active, -1);
  retro_eventcount_notify(&m_cpu_idle);
}

void CPUManager::WaitForIdle()
{
  retro_atomic_thread_fence_seq_cst();
  for (;;)
  {
    if (!retro_atomic_load_seq_cst_int(&m_cpu_thread_active))
      return;
    const int key = retro_eventcount_prepare_wait(&m_cpu_idle);
    if (!retro_atomic_load_seq_cst_int(&m_cpu_thread_active))
    {
      retro_eventcount_cancel_wait(&m_cpu_idle);
      return;
    }
    retro_eventcount_commit_wait(&m_cpu_idle, key);
  }
}

void CPUManager::ClaimStepping()
{
  for (;;)
  {
    if (retro_atomic_cas_int(&m_stepping_claimed, 0, 1))
      return;
    const int key = retro_eventcount_prepare_wait(&m_stepping_released);
    if (!retro_atomic_load_seq_cst_int(&m_stepping_claimed))
    {
      retro_eventcount_cancel_wait(&m_stepping_released);
      continue;
    }
    retro_eventcount_commit_wait(&m_stepping_released, key);
  }
}

void CPUManager::ReleaseStepping()
{
  retro_atomic_store_release_int(&m_stepping_claimed, 0);
  retro_eventcount_notify(&m_stepping_released);
}

void CPUManager::StartTimePlayedTimer()
{
  Common::SetCurrentThreadName("Play Time Tracker");

  // Use a clock that will appropriately ignore suspended system time.
  Common::SteadyAwakeClock timer;
  auto prev_time = timer.now();

  while (true)
  {
    TimePlayed time_played;
    auto curr_time = timer.now();

    // Check that emulation is not paused
    // If the emulation is paused, wait for SetStepping() to reactivate
    const State state = LoadState();
    if (state == State::Running)
    {
      const std::string game_id = SConfig::GetInstance().GetGameID();
      const auto diff_time =
          std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - prev_time);
      time_played.AddTime(game_id, diff_time);
    }
    else if (state == State::Stepping)
    {
      m_time_played_finish_sync.Wait();
      curr_time = timer.now();
    }

    prev_time = curr_time;

    if (LoadState() == State::PowerDown)
      return;

    m_time_played_finish_sync.WaitFor(std::chrono::seconds(30));
  }
}

void CPUManager::Run()
{
  auto& power_pc = m_system.GetPowerPC();

  // Start a separate time tracker thread
  sthread_t* timing = nullptr;
  if (Config::Get(Config::MAIN_TIME_TRACKING))
  {
    timing = sthread_create(Common::ThreadEntry<&CPUManager::StartTimePlayedTimer>, this);
  }

#ifdef __LIBRETRO__
  retro_atomic_store_release_int(&m_cpu_run_state_reached, 1);
#endif
  while (LoadState() != State::PowerDown)
  {
    // Wait out a PauseAndLock: the host thread is standing in for this one.
    for (;;)
    {
      if (!retro_atomic_load_seq_cst_int(&m_paused_and_locked))
        break;
      const int key = retro_eventcount_prepare_wait(&m_cpu_wake);
      if (!retro_atomic_load_seq_cst_int(&m_paused_and_locked))
      {
        retro_eventcount_cancel_wait(&m_cpu_wake);
        break;
      }
      retro_eventcount_commit_wait(&m_cpu_wake, key);
    }
    ExecutePendingJobs();
    CPUThreadConfigCallback::CheckForConfigChanges();

    Common::Event gdb_step_sync_event;
    switch (LoadState())
    {
    case State::Running:
      if (!TryEnterActive(State::Running))
        break;

      // Adjust PC when debugging
      // SingleStep so that the "continue", "step over" and "step out" debugger functions
      // work when the PC is at a breakpoint at the beginning of the block
      // Don't use PowerPCManager::CheckBreakPoints, otherwise you get double logging
      // If watchpoints are enabled, any instruction could be a breakpoint.
      if (power_pc.GetBreakPoints().IsAddressBreakPoint(power_pc.GetPPCState().pc) ||
          power_pc.GetMemChecks().HasAny())
      {
        // Only from Running, so a pause or stop that came in meanwhile is not undone.
        int expected = static_cast<int>(State::Running);
        const bool swapped =
            retro_atomic_cas_int(&m_state, expected, static_cast<int>(State::Stepping));
        PowerPC::CoreMode old_mode = power_pc.GetMode();
        power_pc.SetMode(PowerPC::CoreMode::Interpreter);
        power_pc.SingleStep();
        power_pc.SetMode(old_mode);
        if (swapped)
        {
          expected = static_cast<int>(State::Stepping);
          retro_atomic_cas_int(&m_state, expected, static_cast<int>(State::Running));
        }
      }

      // Enter a fast runloop
      power_pc.RunLoop();

      LeaveActive();
      break;

    case State::Stepping:
      // Wait for step command.
      for (;;)
      {
        ExecutePendingJobs();
        CPUThreadConfigCallback::CheckForConfigChanges();
        if (GDBStub::IsActive() && GDBStub::HasControl())
        {
          if (!GDBStub::JustConnected())
            GDBStub::SendSignal(GDBStub::Signal::Sigtrap);
          GDBStub::ProcessCommands(true);
          // If we are still going to step, emulate the fact we just sent a step command
          if (GDBStub::HasControl())
          {
            // Make sure the previous step by gdb was serviced
            auto* previous = static_cast<Common::Event*>(
                retro_atomic_exchange_ptr(&m_step_request, &gdb_step_sync_event));
            if (previous && previous != STEP_NO_EVENT && previous != &gdb_step_sync_event)
              previous->Set();
          }
        }
        if (retro_atomic_load_acquire_ptr(&m_step_request) || !IsStepping())
          break;
        const int key = retro_eventcount_prepare_wait(&m_cpu_wake);
        if (retro_atomic_load_acquire_ptr(&m_step_request) || !IsStepping())
        {
          retro_eventcount_cancel_wait(&m_cpu_wake);
          break;
        }
        retro_eventcount_commit_wait(&m_cpu_wake, key);
      }
      if (!IsStepping())
      {
        // Signal event if the mode changes.
        FlushStepSyncEvent();
        continue;
      }
      if (retro_atomic_load_seq_cst_int(&m_paused_and_locked))
        continue;

      // Do step
      if (!TryEnterActive(State::Stepping))
        continue;

      power_pc.SingleStep();

      LeaveActive();

      // Update disasm dialog
      FlushStepSyncEvent();
      Host_UpdateDisasmDialog();
      break;

    case State::PowerDown:
      break;
    }
  }

  if (timing)
  {
    m_time_played_finish_sync.Set();
    Common::JoinThread(timing);
  }

  Host_UpdateDisasmDialog();
}

#ifdef __LIBRETRO__
void CPUManager::RunSingleFrame()
{
  auto& system = Core::System::GetInstance();
  auto& power_pc = system.GetPowerPC();

  Core::HostDispatchJobs(system);

  Core::s_stop_frame_step.store(false);

  Core::s_frame_step = true;

  SetState(system, Core::State::Running, false, true);

  power_pc.RunLoop();

  AsyncRequests::GetInstance()->PullEvents();
}
#endif

void CPUManager::RunAdjacentSystems(bool running)
{
  // NOTE: We're assuming these will not try to call Break or SetStepping.
  m_system.GetFifo().EmulatorState(running);
  // Core is responsible for shutting down the sound stream.
  if (LoadState() != State::PowerDown)
    AudioCommon::SetSoundStreamRunning(m_system, running);
}

void CPUManager::Stop()
{
  // Change state and wait for it to be acknowledged.
  // We don't need the stepping claim because State::PowerDown is a priority state which
  // will stick permanently.
  retro_atomic_exchange_int(&m_state, static_cast<int>(State::PowerDown));
  WakeCPU();
  WaitForIdle();

  RunAdjacentSystems(false);
  FlushStepSyncEvent();
}

bool CPUManager::IsStepping() const
{
  return LoadState() == State::Stepping;
}

State CPUManager::GetState() const
{
  return LoadState();
}

const State* CPUManager::GetStatePtr() const
{
  // The JIT reads the word directly; the atomic has the layout of the int it holds.
  static_assert(sizeof(m_state) == sizeof(State));
  return reinterpret_cast<const State*>(&m_state);
}

void CPUManager::Reset()
{
}

void CPUManager::StepOpcode(Common::Event* event)
{
  // If we're not stepping then this is pointless
  if (!IsStepping())
  {
    if (event)
      event->Set();
    return;
  }

  // Potential race where the previous step has not been serviced yet.
  auto* previous = static_cast<Common::Event*>(
      retro_atomic_exchange_ptr(&m_step_request, event ? event : STEP_NO_EVENT));
  if (previous && previous != STEP_NO_EVENT && previous != event)
    previous->Set();
  WakeCPU();
}

// PowerDown sticks: a state change after it is refused.
bool CPUManager::SetStateChecked(State s)
{
  int cur = retro_atomic_load_seq_cst_int(&m_state);
  for (;;)
  {
    if (cur == static_cast<int>(State::PowerDown))
      return false;
    if (retro_atomic_cas_int(&m_state, cur, static_cast<int>(s)))
      break;
    cur = retro_atomic_load_seq_cst_int(&m_state);
  }
  if (s == State::Stepping)
    m_system.GetPowerPC().GetBreakPoints().ClearTemporary();
  return true;
}

void CPUManager::SetStepping(bool stepping)
{
  ClaimStepping();

  if (stepping)
  {
    SetStateChecked(State::Stepping);

    if (!Core::IsCPUThread())
      WaitForIdle();

    RunAdjacentSystems(false);
  }
  else if (SetStateChecked(State::Running))
  {
    if (!Core::IsCPUThread())
      WakeCPU();
    m_time_played_finish_sync.Set();
    RunAdjacentSystems(true);
  }

  ReleaseStepping();
}

void CPUManager::Break()
{
  // If another thread is trying to PauseAndLock then we need to remember this
  // for later to ignore the unpause_on_unlock.
  if (retro_atomic_load_seq_cst_int(&m_paused_and_locked))
  {
    retro_atomic_store_release_int(&m_system_request_stepping, 1);
    return;
  }

  // We'll deadlock if we synchronize, the CPU may block waiting for our caller to
  // finish resulting in the CPU loop never terminating.
  SetStateChecked(State::Stepping);
  RunAdjacentSystems(false);
}

void CPUManager::Continue()
{
  SetStepping(false);
  Core::NotifyStateChanged(Core::State::Running);
}

bool CPUManager::PauseAndLock()
{
  ClaimStepping();

  retro_atomic_exchange_int(&m_paused_and_locked, 1);

  const bool was_unpaused = LoadState() == State::Running;
  SetStateChecked(State::Stepping);

  WaitForIdle();

  // NOTE: It would make more sense for Core::DeclareAsCPUThread() to keep a
  //   depth counter instead of being a boolean.
  if (!Core::IsCPUThread())
  {
    m_have_fake_cpu_thread = true;
    Core::DeclareAsCPUThread();
  }

  return was_unpaused;
}

void CPUManager::RestoreStateAndUnlock(const bool unpause_on_unlock)
{
  // Only need the stepping claim for this
  if (m_have_fake_cpu_thread)
  {
    m_have_fake_cpu_thread = false;
    Core::UndeclareAsCPUThread();
  }

  if (retro_atomic_exchange_int(&m_system_request_stepping, 0))
  {
    // A Break came in while paused: stay stepping.
  }
  else if (unpause_on_unlock)
  {
    SetStateChecked(State::Running);
  }
  retro_atomic_exchange_int(&m_paused_and_locked, 0);
  WakeCPU();

  RunAdjacentSystems(LoadState() == State::Running);

  ReleaseStepping();
}

void CPUManager::AddCPUThreadJob(Common::MoveOnlyFunction<void()> function)
{
  Job* job = new Job{{}, std::move(function)};
  mpsc_stack_push(&m_pending_jobs, &job->link);
}

}  // namespace CPU
