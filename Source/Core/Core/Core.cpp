// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Core.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <variant>

#include <cstdarg>
#include <cstdio>
#include <queues/mpsc_stack.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "AudioCommon/FieldAudio.h"

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"
#include "Common/Version.h"

#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/HW.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/IOS/IOS.h"
#include "Core/MemTools.h"
#include "Core/NetworkCaptureLogger.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"


#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

namespace Core
{
static bool s_wants_determinism;

/* What EmuThread brought up, taken down again in reverse (EmuThreadTeardown): when EmuThread
 * returns on its own thread, or when the boot fails; in single core, where it returns once the
 * game has booted, by Shutdown. */
struct EmuThreadStages
{
  Core::System* system;
  bool state;     // the state goes back to Uninitialized
  bool sd_sync;   // the Wii SD card image is synced back to its folder
  bool hw;        // the hardware is shut down
  bool gpu_loop;  // dual core: the GPU loop is told to stop
};
static EmuThreadStages s_emu_thread_stages; /* single core, until Shutdown */
BootParameters* g_boot_params;
// Declarations and definitions
sthread_t* s_emu_thread = nullptr;

bool s_frame_step = false;

// The value Paused is never stored in this variable. The core is considered to be in
// the Paused state if this variable is Running and the CPU reports that it's stepping.
static retro_atomic_int_t s_state; /* a State; 0 is Uninitialized */

static State LoadState(void)
{
  return static_cast<State>(retro_atomic_load_acquire_int(&s_state));
}

static void StoreState(State state)
{
  retro_atomic_store_release_int(&s_state, static_cast<int>(state));
}

/* Starting becomes Running, unless another thread already set Stopping. */
static void StartingToRunning(void)
{
  retro_atomic_cas_int(&s_state, static_cast<int>(State::Starting),
                       static_cast<int>(State::Running));
}

static thread_local bool tls_is_cpu_thread = false;
static thread_local bool tls_is_gpu_thread = false;

// Display messages and return values

// Formatted stop message
std::string StopMessage(bool main_thread, std::string_view message)
{
  return StringFromFormat("Stop [%s %d]\t%.*s", main_thread ? "Main Thread" : "Video Thread",
                          Common::CurrentThreadId(), static_cast<int>(message.size()),
                          message.data());
}

void DisplayMessage(const char* format, ...)
{
  // Nothing is drawn over the game: the message goes to the log.
  char message[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  NOTICE_LOG(CORE, "%s", message);
}

bool IsRunning(Core::System& system)
{
  return LoadState() == State::Running;
}

bool IsRunningOrStarting(Core::System& system)
{
  const State state = LoadState();
  return state == State::Running || state == State::Starting;
}

bool IsUninitialized(Core::System& system)
{
  return LoadState() == State::Uninitialized;
}

bool IsCPUThread()
{
  return tls_is_cpu_thread;
}

bool WantsDeterminism()
{
  return s_wants_determinism;
}

// This is called from the GUI thread. See the booting call schedule in
// BootManager.cpp
bool Init(Core::System& system, BootParameters* boot)
{
  if (s_emu_thread)
  {
    if (!IsUninitialized(system))
    {
      PanicAlert("Emu Thread already running");
      delete boot;
      return false;
    }

    // The Emu Thread was stopped, synchronize with it.
    Common::JoinThread(s_emu_thread);
  }

  INFO_LOG(BOOT, "Starting core = %s mode", system.IsWii() ? "Wii" : "GameCube");
  INFO_LOG(BOOT, "CPU Thread separate = %s", system.IsDualCoreMode() ? "Yes" : "No");

  // Manually reactivate the video backend in case a GameINI overrides the video backend setting.

  // Start the emu thread
  StoreState(State::Starting);

  // Do not launch EmuThread — Libretro handles it manually
  delete g_boot_params;
  g_boot_params = boot;

  return true;
}

static void ResetRumble()
{
  GCAdapter::ResetRumble();
  if (!Pad::IsInitialized())
    return;
  for (int i = 0; i < 4; ++i)
    Pad::ResetRumble(i);
}

// Called from GUI thread
void Stop(Core::System& system)  // - Hammertime!
{
  State state;
  do
  {
    state = LoadState();
    if (state == State::Stopping || state == State::Uninitialized)
      return;
  } while (
      !retro_atomic_cas_int(&s_state, static_cast<int>(state), static_cast<int>(State::Stopping)));

  system.GetFifo().EmulatorState(false);

  INFO_LOG(CONSOLE, "Stop [Main Thread]\t\t---- Shutting down ----");

  // Stop the CPU
  INFO_LOG(CONSOLE, "%s", StopMessage(true, "Stop CPU").c_str());
  system.GetCPU().Stop();
}

void DeclareAsCPUThread()
{
  tls_is_cpu_thread = true;
}

void UndeclareAsCPUThread()
{
  tls_is_cpu_thread = false;
}

void DeclareAsGPUThread()
{
  tls_is_gpu_thread = true;
}

void UndeclareAsGPUThread()
{
  tls_is_gpu_thread = false;
}

// For the CPU Thread only.
static void CPUSetInitialExecutionState(Core::System& system)
{
  // The CPU starts in stepping state, and will wait until a new state is set before executing.
  const bool paused = SConfig::GetInstance().bBootToPause;
  SetState(system, paused ? State::Paused : State::Running, true, true);
}

// Create the CPU thread, which is a CPU + Video thread in Single Core mode.
static void CpuThread(Core::System& system)
{
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("CPU thread");
  else
    Common::SetCurrentThreadName("CPU-GPU thread");

  // Clear performance data collected from previous threads.

  // The JIT need to be able to intercept faults, both for fastmem and for the BLR optimization.
  const bool exception_handler = EMM::IsExceptionHandlerSupported();
  if (exception_handler)
    EMM::InstallExceptionHandler();

  StartingToRunning();

  CPUSetInitialExecutionState(system);

  // dont enter the blocking loop if in single core mode
  if (!system.IsDualCoreMode())
    return;
  // Enter CPU run loop. When we leave it - we are done.
  system.GetCPU().Run();

  if (exception_handler)
    EMM::UninstallExceptionHandler();
}

/* In single core, the FIFO player's CPU core stays injected after the
 * thread function returns: RunSingleFrame runs it a field at a time, and
 * it is taken out at unload, before the hardware it drives is shut down. */
static CPUCoreBase* s_fifo_cpu_core;

static void DetachFifoCPUCore()
{
  Core::System& system = Core::System::GetInstance();

  system.GetPowerPC().InjectExternalCPUCore(nullptr);
  s_fifo_cpu_core = nullptr;
  system.GetFifoPlayer().Close();
}

static void FifoPlayerThread(Core::System& system)
{
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("FIFO player thread");
  else
    Common::SetCurrentThreadName("FIFO-GPU thread");

  // Enter CPU run loop. When we leave it - we are done.
  if (CPUCoreBase* const cpu_core = system.GetFifoPlayer().GetCPUCore())
  {
    system.GetPowerPC().InjectExternalCPUCore(cpu_core);

    StartingToRunning();

    CPUSetInitialExecutionState(system);

    // dont enter the blocking loop if in single core mode
    if (!system.IsDualCoreMode())
    {
      s_fifo_cpu_core = cpu_core;
      return;
    }

    system.GetCPU().Run();

    system.GetPowerPC().InjectExternalCPUCore(nullptr);
    system.GetFifoPlayer().Close();
  }
  else
  {
    PanicAlert("FIFO file doesn't contain any frame, cannot playback.");
    system.GetFifoPlayer().Close();
    return;
  }
}

// Readies the video side for the CPU thread: true when the GPU loop is to be stopped at the end.
static bool InitializeVideoThreads(Core::System& system)
{
  if (system.IsDualCoreMode())
  {
    // The frontend runs the GPU loop on its own thread; all that is left here is to make sure
    // that loop is told to stop.
    AsyncRequests::GetInstance()->SetPassthrough(false);
    return true;
  }

  DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(true);
  return false;
}

static void EmuThreadTeardown(EmuThreadStages* stages)
{
  Core::System& system = *stages->system;
  if (stages->gpu_loop)
  {
    INFO_LOG(CONSOLE, "%s", StopMessage(true, "Wait for Video Loop to exit ...").c_str());
    system.GetFifo().ExitGpuLoop();
    INFO_LOG(CONSOLE, "%s", StopMessage(true, "GPU thread stopped.").c_str());
  }
  if (stages->hw)
  {
    INFO_LOG(CONSOLE, "%s", StopMessage(false, "Shutting down HW").c_str());
    HW::Shutdown(system);
    INFO_LOG(CONSOLE, "%s", StopMessage(false, "HW shutdown").c_str());

    // The config must be restored only after the whole HW has shut down,
    // not when it is still running.
    BootManager::RestoreConfig();

    PatchEngine::Shutdown();
    HLE::Clear();
    ResetNetworkLogger();
  }
  if (stages->sd_sync && Config::Get(Config::MAIN_ALLOW_SD_WRITES))
  {
    const bool sync_ok = Common::SyncSDImageToSDFolder();
    if (!sync_ok)
    {
      PanicAlert("Failed to sync SD card with folder. All changes made this session will be "
                 "discarded on next boot if you do not manually re-issue a resync in Config > "
                 "Wii > SD Card Settings > %s!",
                 Common::GetStringT(Common::SD_UNPACK_TEXT).c_str());
    }
  }
  if (stages->state)
  {
    StoreState(State::Uninitialized);

    INFO_LOG(CONSOLE, "Stop\t\t---- Shutdown complete ----");
  }
  *stages = EmuThreadStages{};
}

namespace
{
struct EmuThreadArgs
{
  Core::System* system;
  BootParameters* boot;  // handed to the thread
};

void EmuThreadEntry(void* data)
{
  const EmuThreadArgs args = *static_cast<EmuThreadArgs*>(data);
  delete static_cast<EmuThreadArgs*>(data);
  EmuThread(*args.system, args.boot);
}
}  // namespace

/* The frame gate (dual core): the CPU thread runs one field per retro_run.
 * At a field's end it counts the field ended, lets the GPU loop finish it,
 * and parks until the frontend's thread grants the next; between two
 * retro_runs it runs nothing. The field the thread boots into counts as
 * granted. A request that stops the CPU thread (a pause, a savestate,
 * shutdown) kicks it out of the gate, where it stops just as a break at
 * the field's end would; resumed, it waits at the gate again before it
 * runs on, so it never runs a field the frontend did not ask for. */
static retro_atomic_int_t s_fields_granted; /* the frontend's thread */
static retro_atomic_int_t s_fields_ended;   /* the CPU thread */
static retro_eventcount_t s_frame_gate;
static bool s_frame_gate_ready;
static bool s_frame_gate_owed; /* the CPU thread: it left the gate ungranted */

static bool FieldGranted()
{
  return retro_atomic_load_acquire_int(&s_fields_granted) -
             retro_atomic_load_acquire_int(&s_fields_ended) >
         0;
}

bool StartEmuThread(Core::System& system, BootParameters* boot)
{
  if (!s_frame_gate_ready)
    s_frame_gate_ready = retro_eventcount_init(&s_frame_gate);
  retro_atomic_store_release_int(&s_fields_granted, 1);
  retro_atomic_store_release_int(&s_fields_ended, 0);
  s_frame_gate_owed = false;

  EmuThreadArgs* const args = new EmuThreadArgs{&system, boot};
  s_emu_thread = sthread_create(EmuThreadEntry, args);
  if (s_emu_thread)
    return true;
  delete args;
  delete boot;
  return false;
}

// Initialize and create emulation thread
// Call browser: Init():s_emu_thread().
// See the BootManager.cpp file description for a complete call schedule.
void EmuThread(Core::System& system, BootParameters* boot)
{
  // taken down on every return but the one single core leaves to Shutdown
  struct Stages
  {
    EmuThreadStages stages{};
    ~Stages() { EmuThreadTeardown(&stages); }
  } emu;
  emu.stages.system = &system;
  emu.stages.state = true;

  Common::SetCurrentThreadName("Emuthread - Starting");

  // This will become the CPU thread.
  DeclareAsCPUThread();

  s_frame_step = false;

  // If settings have changed since the previous run, notify callbacks.
  CPUThreadConfigCallback::CheckForConfigChanges();

  ASSERT(g_controller_interface.IsInit());

  Pad::LoadConfig();
  Pad::LoadGBAConfig();
  Keyboard::LoadConfig();

  BootSessionData boot_session_data = std::move(boot->boot_session_data);

  bool sync_sd_folder = system.IsWii() && Config::Get(Config::MAIN_WII_SD_CARD) &&
                        Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  if (sync_sd_folder)
  {
    sync_sd_folder = Common::SyncSDFolderToSDImage(Core::WantsDeterminism());
  }

  emu.stages.sd_sync = sync_sd_folder;

  // Wiimote input config is loaded in OnESTitleChanged

  HW::Init(system, nullptr);
  emu.stages.hw = true;

  // In dual-core mode the GPU loop is stopped at the end (which does the backend shutdown).
  emu.stages.gpu_loop = InitializeVideoThreads(system);

  if (cpu_info.HTT)
    Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 4);
  else
    Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 2);

  if (!system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(),
                                                    Config::Get(Config::MAIN_DSP_THREAD)))
  {
    PanicAlert("Failed to initialize DSP emulation!");
    delete boot;
    return;
  }

  // Set execution state to known values (CPU/FIFO/Audio Paused)
  system.GetCPU().Break();

  // Load GCM/DOL/ELF whatever ... we boot with the interpreter core
  system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);

  // Determine the CPU thread function
  const auto cpu_thread_func =
      std::holds_alternative<BootParameters::DFF>(boot->parameters) ? FifoPlayerThread : CpuThread;

  {
    ASSERT(IsCPUThread());
    if (!CBoot::BootUp(system, boot))
      return;
  }

  // Initialise Wii filesystem contents.
  // This is done here after Boot and not in BootManager to ensure that we operate
  // with the correct title context since save copying requires title directories to exist.
  if (system.IsWii())
    Core::InitializeWiiFileSystemContents(boot_session_data);

  // This adds the SyncGPU handler to CoreTiming, so now CoreTiming::Advance might block.
  system.GetFifo().Prepare();

  // Setup our core
  if (Config::Get(Config::MAIN_CPU_CORE) != PowerPC::CPUCore::Interpreter)
  {
    system.GetPowerPC().SetMode(PowerPC::CoreMode::JIT);
  }
  else
  {
    system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
  }

  // Single core returns here once the game has booted: Shutdown takes it all down.
  if (!s_emu_thread)
  {
    s_emu_thread_stages = emu.stages;
    emu.stages = EmuThreadStages{};
  }

  // Become the CPU thread.
  cpu_thread_func(system);
}

// Set or get the running state

void SetState(Core::System& system, State state, bool report_state_change,
              bool override_achievement_restrictions)
{
  {
    // State cannot be controlled until the CPU Thread is operational
    if (LoadState() != State::Running)
      return;

    switch (state)
    {
    case State::Paused:
      // NOTE: GetState() will return State::Paused immediately, even before anything has
      //   stopped (including the CPU).
      system.GetCPU().SetStepping(true);  // Break
      Wiimote::Pause();
      ResetRumble();
      break;
    case State::Running:
    {
      system.GetCPU().SetStepping(false);
      /* a frame step runs for one frame only: nothing for the remotes to
       * resume, and nothing posted to their manager every frame */
      if (report_state_change)
        Wiimote::Resume();
      break;
    }
    default:
      PanicAlert("Invalid state");
      break;
    }
  }
}

State GetState(Core::System& system)
{
  const State state = LoadState();
  if (state == State::Running && system.GetCPU().IsStepping())
    return State::Paused;
  else
    return state;
}

void RunOnCPUThread(Core::System& system, void (*run)(void* context), void* context)
{
  retro_atomic_int_t done;
  CPU::Job job;

  // With no CPU thread, nothing runs it but the caller.
  if (IsCPUThread() || !IsRunning(system))
  {
    run(context);
    return;
  }

  retro_atomic_int_init(&done, 0);
  job.link.next = nullptr;
  job.run = run;
  job.context = context;
  job.done = &done;
  system.GetCPU().AddCPUThreadJob(&job);
  system.GetCPU().WaitForJob(&done);
}

// --- Callbacks for backends / engine ---

// Called from VideoInterface::Update (CPU thread) at emulated field boundaries
/* The field's audio ends here: its length is the emulated time since the last field at the
 * output rate, the remainder carried so no sample is gained or lost over time. CPU thread. */
static void SealFieldAudio(Core::System& system)
{
  static u64 s_last_ticks = 0;
  static u64 s_remainder = 0;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  const u64 per_second = system.GetSystemTimers().GetTicksPerSecond();
  const u64 divisor = field_audio_output_divisor();
  u64 scaled, denominator, frames;

  if (ticks < s_last_ticks || per_second == 0 || divisor == 0)
  {
    /* A loaded state or a new boot: the time since the last field means nothing. */
    s_last_ticks = ticks;
    s_remainder = 0;
    field_audio_seal(0);
    return;
  }
  scaled = (ticks - s_last_ticks) * FIELD_AUDIO_RATE_DIVIDEND + s_remainder;
  denominator = per_second * divisor;
  frames = scaled / denominator;
  s_remainder = scaled % denominator;
  s_last_ticks = ticks;
  if (frames > FIELD_AUDIO_MAX_FRAMES)
    frames = FIELD_AUDIO_MAX_FRAMES;
  field_audio_seal((unsigned)frames);
}

void Callback_NewField(Core::System& system)
{
  if (s_frame_step)
  {
    s_frame_step = false;
    system.GetCPU().Break();
  }


  // Sealed before the GPU loop is let go, so the retro_run this field ends finds it.
  SealFieldAudio(system);

  if (system.IsDualCoreMode())
  {
    /* fields before the CPU thread reaches its run loop are boot work
     * (the FIFO player clearing the EFB), not the frontend's frames */
    const bool gated = system.GetCPU().HasCPURunStateBeenReached();
    /* Counted ended before the GPU loop is let go: the frontend's thread
     * learns of the field from the GPU loop, and the next retro_run must
     * find it ended, or it grants nothing and waits for a field the CPU
     * thread, parked at the gate, never runs. */
    if (gated)
      retro_atomic_fetch_add_int(&s_fields_ended, 1);
    system.GetFifo().StopGpuLoop();
    if (gated)
      FrameGateWait(system);
  }
}

/* The CPU thread, at a field's end or resumed with a field owed: parks
 * until the next field is granted or the thread is asked to stop. Resumed,
 * outside the run loop, it also leaves for a job queued for it: a pause
 * that comes and goes before the parked thread wakes leaves a running
 * state behind and a job only this thread can run. */
void FrameGateWait(Core::System& system, bool leave_for_jobs)
{
  auto& cpu = system.GetCPU();
  if (!s_frame_gate_ready)
    return;
  for (;;)
  {
    int key;
    if (FieldGranted())
    {
      s_frame_gate_owed = false;
      return;
    }
    if (cpu.GetState() != CPU::State::Running || (leave_for_jobs && cpu.HasPendingJobs()))
    {
      s_frame_gate_owed = true;
      return;
    }
    key = retro_eventcount_prepare_wait(&s_frame_gate);
    if (FieldGranted() || cpu.GetState() != CPU::State::Running ||
        (leave_for_jobs && cpu.HasPendingJobs()))
    {
      retro_eventcount_cancel_wait(&s_frame_gate);
      continue;
    }
    retro_eventcount_commit_wait(&s_frame_gate, key);
  }
}

bool FrameGateOwed()
{
  return s_frame_gate_owed;
}

void FrameGateKick()
{
  if (s_frame_gate_ready)
    retro_eventcount_notify(&s_frame_gate);
}

void Shutdown(Core::System& system)
{
  // During shutdown DXGI expects us to handle some messages on the UI thread.
  // Therefore we can't immediately block and wait for the emu thread to shut
  // down, so we join the emu thread as late as possible when the UI has already
  // shut down.
  // For more info read "DirectX Graphics Infrastructure (DXGI): Best Practices"
  // on MSDN.
  Common::JoinThread(s_emu_thread);
  if (s_fifo_cpu_core)
    DetachFifoCPUCore();
  if (s_emu_thread_stages.system)
    EmuThreadTeardown(&s_emu_thread_stages);
}

void UpdateWantDeterminism(Core::System& system, bool initial)
{
  // For now, this value is not itself configurable.  Instead, individual
  // settings that depend on it, such as GPU determinism mode. should have
  // override options for testing,
  // Only a movie wanted determinism, and none is ever recorded or played.
  const bool new_want_determinism = false;
  if (new_want_determinism != s_wants_determinism || initial)
  {
    NOTICE_LOG(COMMON, "Want determinism <- %s", new_want_determinism ? "true" : "false");

    s_wants_determinism = new_want_determinism;
    const auto ios = system.GetIOS();
    if (ios)
      ios->UpdateWantDeterminism(new_want_determinism);

    system.GetFifo().UpdateWantDeterminism(new_want_determinism);

    // We need to clear the cache because some parts of the JIT depend on want_determinism,
    // e.g. use of FMA.
    system.GetJitInterface().ClearCache();
  }
}

bool IsFieldInFlight()
{
  return FieldGranted();
}

// The frontend's thread, retro_run: the next field, unless one is still running (a wedge left
// it to this run); a CPU thread something paused is let go again.
void RunField(Core::System& system)
{
  if (!FieldGranted())
  {
    retro_atomic_fetch_add_int(&s_fields_granted, 1);
    FrameGateKick();
  }
  if (GetState(system) == State::Paused)
    SetState(system, State::Running, false);
}

void UpdateInputGate(bool require_focus, bool require_full_focus)
{
  // If the user accepts background input, controls should pass even if an on screen interface is on
  const bool focus_passes =
      !require_focus ||
      ((Host_RendererHasFocus() || Host_TASInputHasFocus()) && !Host_UIBlocksControllerState());
  // Ignore full focus if we don't require basic focus
  const bool full_focus_passes =
      !require_focus || !require_full_focus || (focus_passes && Host_RendererHasFullFocus());
  ControlReference::SetInputGate(focus_passes && full_focus_passes);
}

void SingleCorePostRunShutdown()
{
  if (EMM::IsExceptionHandlerSupported())
    EMM::UninstallExceptionHandler();
}

}  // namespace Core
