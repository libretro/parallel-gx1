// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Core.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include <fmt/chrono.h>
#include <fmt/format.h>
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
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Event.h"
#include "Common/ScopeGuard.h"
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

static std::vector<Common::ScopeGuard<Common::MoveOnlyFunction<void()>>> s_emu_thread_scope_guards;
std::unique_ptr<BootParameters> g_boot_params;
// Declarations and definitions
sthread_t* s_emu_thread = nullptr;
static Common::HookableEvent<Core::State> s_state_changed_event;

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

struct HostJob
{
  mpsc_stack_node_t link;  // first: the drained chain is cast back to jobs
  std::function<void(Core::System&)> job;
  bool run_after_stop;
};
// Pushed from any thread; the host thread moves them in order into s_host_jobs_ready.
static mpsc_stack_t s_host_jobs_queue;
static std::deque<std::unique_ptr<HostJob>> s_host_jobs_ready;

static thread_local bool tls_is_cpu_thread = false;
static thread_local bool tls_is_gpu_thread = false;

// Display messages and return values

// Formatted stop message
std::string StopMessage(bool main_thread, std::string_view message)
{
  return fmt::format("Stop [{} {}]\t{}", main_thread ? "Main Thread" : "Video Thread",
                     Common::CurrentThreadId(), message);
}

void DisplayMessage(std::string message, int)
{
  // Nothing is drawn over the game: the message goes to the log.
  NOTICE_LOG_FMT(CORE, "{}", message);
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

bool IsGPUThread()
{
  return tls_is_gpu_thread;
}

bool WantsDeterminism()
{
  return s_wants_determinism;
}

// This is called from the GUI thread. See the booting call schedule in
// BootManager.cpp
bool Init(Core::System& system, std::unique_ptr<BootParameters> boot)
{
  if (s_emu_thread)
  {
    if (!IsUninitialized(system))
    {
      PanicAlertFmtT("Emu Thread already running");
      return false;
    }

    // The Emu Thread was stopped, synchronize with it.
    Common::JoinThread(s_emu_thread);
  }

  // Drain any left over jobs
  HostDispatchJobs(system);

  INFO_LOG_FMT(BOOT, "Starting core = {} mode", system.IsWii() ? "Wii" : "GameCube");
  INFO_LOG_FMT(BOOT, "CPU Thread separate = {}", system.IsDualCoreMode() ? "Yes" : "No");

  // Manually reactivate the video backend in case a GameINI overrides the video backend setting.

  // Start the emu thread
  StoreState(State::Starting);

  // Do not launch EmuThread — Libretro handles it manually
  g_boot_params = std::move(boot);

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

  NotifyStateChanged(State::Stopping);


  // Dump left over jobs
  HostDispatchJobs(system);

  system.GetFifo().EmulatorState(false);

  INFO_LOG_FMT(CONSOLE, "Stop [Main Thread]\t\t---- Shutting down ----");

  // Stop the CPU
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Stop CPU"));
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
static void CpuThread(Core::System& system, const std::optional<std::string> savestate_path,
                      bool delete_savestate)
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

  if (savestate_path)
  {
    ::State::LoadAs(system, *savestate_path);
    if (delete_savestate)
      File::Delete(*savestate_path);
  }

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
static std::unique_ptr<CPUCoreBase> s_fifo_cpu_core;

static void DetachFifoCPUCore()
{
  Core::System& system = Core::System::GetInstance();

  system.GetPowerPC().InjectExternalCPUCore(nullptr);
  s_fifo_cpu_core.reset();
  system.GetFifoPlayer().Close();
}

static void FifoPlayerThread(Core::System& system, const std::optional<std::string> savestate_path,
                             bool delete_savestate)
{
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("FIFO player thread");
  else
    Common::SetCurrentThreadName("FIFO-GPU thread");

  // Enter CPU run loop. When we leave it - we are done.
  if (auto cpu_core = system.GetFifoPlayer().GetCPUCore())
  {
    system.GetPowerPC().InjectExternalCPUCore(cpu_core.get());

    StartingToRunning();

    CPUSetInitialExecutionState(system);

    // dont enter the blocking loop if in single core mode
    if (!system.IsDualCoreMode())
    {
      s_fifo_cpu_core = std::move(cpu_core);
      return;
    }

    system.GetCPU().Run();

    system.GetPowerPC().InjectExternalCPUCore(nullptr);
    system.GetFifoPlayer().Close();
  }
  else
  {
    PanicAlertFmt("FIFO file doesn't contain any frame, cannot playback.");
    system.GetFifoPlayer().Close();
    return;
  }
}

// Returns a RAII object for video backend initialization and deinitialization.
// Returns nullptr on failure.
[[nodiscard]] static auto GetInitializedVideoGuard(Core::System& system)
{
  using GuardType = Common::ScopeGuard<Common::MoveOnlyFunction<void()>>;

  if (system.IsDualCoreMode())
  {
    // The frontend runs the GPU loop on its own thread; all that is left here is to make sure
    // that loop is told to stop.
    AsyncRequests::GetInstance()->SetPassthrough(false);
    return std::make_unique<GuardType>([&system] {
      INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Wait for Video Loop to exit ..."));
      system.GetFifo().ExitGpuLoop();
      INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "GPU thread stopped."));
    });
  }

  DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(true);
  return std::make_unique<GuardType>([] {});
}

namespace
{
struct EmuThreadArgs
{
  Core::System* system;
  std::unique_ptr<BootParameters> boot;
};

void EmuThreadEntry(void* data)
{
  std::unique_ptr<EmuThreadArgs> args(static_cast<EmuThreadArgs*>(data));
  EmuThread(*args->system, std::move(args->boot));
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

bool StartEmuThread(Core::System& system, std::unique_ptr<BootParameters> boot)
{
  if (!s_frame_gate_ready)
    s_frame_gate_ready = retro_eventcount_init(&s_frame_gate);
  retro_atomic_store_release_int(&s_fields_granted, 1);
  retro_atomic_store_release_int(&s_fields_ended, 0);
  s_frame_gate_owed = false;

  EmuThreadArgs* const args = new EmuThreadArgs{&system, std::move(boot)};
  s_emu_thread = sthread_create(EmuThreadEntry, args);
  if (s_emu_thread)
    return true;
  delete args;
  return false;
}

// Initialize and create emulation thread
// Call browser: Init():s_emu_thread().
// See the BootManager.cpp file description for a complete call schedule.
void EmuThread(Core::System& system, std::unique_ptr<BootParameters> boot)
{
  NotifyStateChanged(State::Starting);
  Common::ScopeGuard flag_guard{[] {
    StoreState(State::Uninitialized);

    NotifyStateChanged(State::Uninitialized);

    INFO_LOG_FMT(CONSOLE, "Stop\t\t---- Shutdown complete ----");
  }};

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
  const std::optional<std::string> savestate_path = boot_session_data.GetSavestatePath();
  const bool delete_savestate =
      boot_session_data.GetDeleteSavestate() == DeleteSavestateAfterBoot::Yes;

  bool sync_sd_folder = system.IsWii() && Config::Get(Config::MAIN_WII_SD_CARD) &&
                        Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  if (sync_sd_folder)
  {
    sync_sd_folder = Common::SyncSDFolderToSDImage([] { return false; }, Core::WantsDeterminism());
  }

  Common::ScopeGuard sd_folder_sync_guard{[sync_sd_folder] {
    if (sync_sd_folder && Config::Get(Config::MAIN_ALLOW_SD_WRITES))
    {
      const bool sync_ok = Common::SyncSDImageToSDFolder([] { return false; });
      if (!sync_ok)
      {
        PanicAlertFmtT(
            "Failed to sync SD card with folder. All changes made this session will be "
            "discarded on next boot if you do not manually re-issue a resync in Config > "
            "Wii > SD Card Settings > {0}!",
            Common::GetStringT(Common::SD_UNPACK_TEXT));
      }
    }
  }};

  // Wiimote input config is loaded in OnESTitleChanged

  HW::Init(system, nullptr);

  Common::ScopeGuard hw_guard{[&system] {
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "Shutting down HW"));
    HW::Shutdown(system);
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "HW shutdown"));

    // The config must be restored only after the whole HW has shut down,
    // not when it is still running.
    BootManager::RestoreConfig();

    PatchEngine::Shutdown();
    HLE::Clear();
    ResetNetworkLogger();
  }};

  // In single-core mode: This holds a video backend shutdown function.
  // In dual-core mode: This holds a GPU thread stopping function (which does the backend shutdown).
  const auto video_guard = GetInitializedVideoGuard(system);
  if (!video_guard)
  {
    PanicAlertFmt("Failed to initialize video backend!");
    return;
  }

  if (cpu_info.HTT)
    Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 4);
  else
    Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > 2);

  if (!system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(),
                                                    Config::Get(Config::MAIN_DSP_THREAD)))
  {
    PanicAlertFmt("Failed to initialize DSP emulation!");
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
    CPUThreadGuard guard(system);
    if (!CBoot::BootUp(system, guard, std::move(boot)))
      return;
  }

  // Initialise Wii filesystem contents.
  // This is done here after Boot and not in BootManager to ensure that we operate
  // with the correct title context since save copying requires title directories to exist.
  auto bsd_ptr = std::make_shared<BootSessionData>(std::move(boot_session_data));
  Common::ScopeGuard wiifs_guard {
    [bsd_ptr]() {
      if (!bsd_ptr)
        return;
      Core::CleanUpWiiFileSystemContents(*bsd_ptr);
    }
  };
  if (system.IsWii())
    Core::InitializeWiiFileSystemContents(boot_session_data);
  else
    wiifs_guard.Dismiss();

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

  UpdateTitle(system);

  // these are not needed if EmuThread is launched on its own thread
  if (!s_emu_thread)
  {
    s_emu_thread_scope_guards.emplace_back(Common::MoveOnlyFunction<void()>(
      [g = std::move(flag_guard)]() mutable { g.Exit(); }));

    s_emu_thread_scope_guards.emplace_back(Common::MoveOnlyFunction<void()>(
      [g = std::move(hw_guard)]() mutable { g.Exit(); }));

    if (system.IsDualCoreMode() && video_guard)
    {
      auto* vg_ptr = video_guard.get();
      s_emu_thread_scope_guards.emplace_back(Common::MoveOnlyFunction<void()>(
        [vg_ptr]() {
          if (vg_ptr) vg_ptr->Exit();
      }));
    }

    s_emu_thread_scope_guards.emplace_back(Common::MoveOnlyFunction<void()>(
      [g = std::move(wiifs_guard)]() mutable { g.Exit(); }));
  }

  // Become the CPU thread.
  cpu_thread_func(system, savestate_path, delete_savestate);
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
      PanicAlertFmt("Invalid state");
      break;
    }
  }

  // Certain callers only change the state momentarily. Sending a callback for them causes
  // unwanted updates, such as the Pause/Play button flickering between states on frame advance.
  if (report_state_change)
    NotifyStateChanged(GetState(system));
}

State GetState(Core::System& system)
{
  const State state = LoadState();
  if (state == State::Running && system.GetCPU().IsStepping())
    return State::Paused;
  else
    return state;
}

static bool PauseAndLock(Core::System& system)
{
  if (!IsRunning(system))
    return true;

  // First pause the CPU.  This acquires a wrapper mutex and converts the current thread into
  // a temporary replacement CPU Thread.
  const bool was_unpaused = system.GetCPU().PauseAndLock();

  // audio has to come after CPU, because CPU thread can wait for audio thread (m_throttle).
  system.GetDSP().GetDSPEmulator()->PauseAndLock();

  // video has to come after CPU, because CPU thread can wait for video thread
  // (s_efbAccessRequested).
  system.GetFifo().PauseAndLock();

  ResetRumble();

  return was_unpaused;
}

static void RestoreStateAndUnlock(Core::System& system, const bool unpause_on_unlock)
{
  if (!IsRunning(system))
    return;

  system.GetDSP().GetDSPEmulator()->UnpauseAndUnlock();
  ResetRumble();

  // CPU is unlocked last because CPU::RestoreStateAndUnlock contains the synchronization mechanism
  // that prevents CPU::Break from racing.
  //
  // The CPU is responsible for managing the Audio and FIFO state so we use its mechanism to unpause
  // them. If we unpaused the systems above when releasing the locks then they could call CPU::Break
  // which would require detecting it and re-pausing with CPU::SetStepping.
  system.GetCPU().RestoreStateAndUnlock(unpause_on_unlock);
}

void RunOnCPUThread(Core::System& system, Common::MoveOnlyFunction<void()> function
                    , bool wait_for_completion
                  )
{
  if (IsCPUThread())
  {
    function();
    return;
  }

  Common::Event cpu_thread_job_finished;

  // Pause the CPU (set it to stepping mode).
  const bool was_running = PauseAndLock(system);

  if (!IsRunning(system))
  {
    // If the core hasn't been started, there is no active CPU thread we can race against.
    function();
    wait_for_completion = false;
  }
  else if (wait_for_completion)
  {
    // Queue the job function followed by triggering the event.
    system.GetCPU().AddCPUThreadJob([&function, &cpu_thread_job_finished] {
      function();
      cpu_thread_job_finished.Set();
    });
  }
  else
  {
    // Queue the job function.
    system.GetCPU().AddCPUThreadJob(std::move(function));
  }

  // Release the CPU thread, and let it execute the callback.
  RestoreStateAndUnlock(system, was_running);

  // If we're waiting for completion, block until the event fires.
  if (wait_for_completion)
  {
    cpu_thread_job_finished.Wait();
  }
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
    NotifyStateChanged(Core::GetState(system));
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
 * until the next field is granted or the thread is asked to stop. */
void FrameGateWait(Core::System& system)
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
    if (cpu.GetState() != CPU::State::Running)
    {
      s_frame_gate_owed = true;
      return;
    }
    key = retro_eventcount_prepare_wait(&s_frame_gate);
    if (FieldGranted() || cpu.GetState() != CPU::State::Running)
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

void UpdateTitle(Core::System& system)
{
  // Settings are shown the same for both extended and summary info
  const std::string SSettings = fmt::format(
      "{} {} | {} | {}", system.GetPowerPC().GetCPUName(), system.IsDualCoreMode() ? "DC" : "SC",
      g_video_backend->GetDisplayName(), Config::Get(Config::MAIN_DSP_HLE) ? "HLE" : "LLE");

  std::string message = fmt::format("{} | {}", Common::GetScmRevStr(), SSettings);
  if (Config::Get(Config::MAIN_SHOW_ACTIVE_TITLE))
  {
    const std::string& title = SConfig::GetInstance().GetTitleDescription();
    if (!title.empty())
      message += " | " + title;
  }

  Host_UpdateTitle(message);
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
  for (auto& g : s_emu_thread_scope_guards)
    g.Exit();
  s_emu_thread_scope_guards.clear();

  // Make sure there's nothing left over in case we're about to exit.
  HostDispatchJobs(system);
}

Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback)
{
  return s_state_changed_event.Register(std::move(callback));
}

void NotifyStateChanged(const Core::State state)
{
  s_state_changed_event.Trigger(state);
}

void UpdateWantDeterminism(Core::System& system, bool initial)
{
  const Core::CPUThreadGuard guard(system);

  // For now, this value is not itself configurable.  Instead, individual
  // settings that depend on it, such as GPU determinism mode. should have
  // override options for testing,
  // Only a movie wanted determinism, and none is ever recorded or played.
  const bool new_want_determinism = false;
  if (new_want_determinism != s_wants_determinism || initial)
  {
    NOTICE_LOG_FMT(COMMON, "Want determinism <- {}", new_want_determinism ? "true" : "false");

    s_wants_determinism = new_want_determinism;
    const auto ios = system.GetIOS();
    if (ios)
      ios->UpdateWantDeterminism(new_want_determinism);

    system.GetFifo().UpdateWantDeterminism(new_want_determinism);

    // We need to clear the cache because some parts of the JIT depend on want_determinism,
    // e.g. use of FMA.
    system.GetJitInterface().ClearCache(guard);
  }
}

void QueueHostJob(std::function<void(Core::System&)> job, bool run_during_stop)
{
  if (!job)
    return;

  auto* host_job = new HostJob{{}, std::move(job), run_during_stop};
  mpsc_stack_push(&s_host_jobs_queue, &host_job->link);
  // If the the queue was empty then kick the Host to come and get this job.
  if (!host_job->link.next)
    Host_Message(HostMessageID::WMUserJobDispatch);
}

void HostDispatchJobs(Core::System& system)
{
  // WARNING: This should only run on the Host Thread.
  // NOTE: This function is potentially re-entrant. If a job calls
  //   Core::Stop for instance then we'll enter this a second time.
  for (;;)
  {
    // The stack gives the newest first; append them oldest first behind what is left.
    const size_t at = s_host_jobs_ready.size();
    for (mpsc_stack_node_t* node = mpsc_stack_drain(&s_host_jobs_queue); node;)
    {
      mpsc_stack_node_t* next = node->next;
      s_host_jobs_ready.emplace(s_host_jobs_ready.begin() + at,
                                reinterpret_cast<HostJob*>(node));
      node = next;
    }
    if (s_host_jobs_ready.empty())
      return;

    std::unique_ptr<HostJob> job = std::move(s_host_jobs_ready.front());
    s_host_jobs_ready.pop_front();

    if (!job->run_after_stop)
    {
      const State state = LoadState();
      if (state == State::Stopping || state == State::Uninitialized)
        continue;
    }

    job->job(system);
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

CPUThreadGuard::CPUThreadGuard(Core::System& system)
    : m_system(system), m_was_cpu_thread(IsCPUThread())
{
  if (!m_was_cpu_thread)
    m_was_unpaused = PauseAndLock(system);
}

CPUThreadGuard::~CPUThreadGuard()
{
  if (!m_was_cpu_thread)
    RestoreStateAndUnlock(m_system, m_was_unpaused);
}

void SingleCorePostRunShutdown()
{
  if (EMM::IsExceptionHandlerSupported())
    EMM::UninstallExceptionHandler();
}

}  // namespace Core
