// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Core

// The external interface to the emulator core. Plus some extras.
// This is another part of the emu that needs cleaning - Core.cpp really has
// too much random junk inside.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <rthreads/rthreads.h>

#include "Common/CommonTypes.h"

struct BootParameters;

namespace Core
{
class System;


void Callback_NewField(Core::System& system);

enum class State
{
  Uninitialized,
  Paused,
  Running,
  Stopping,
  Starting,
};

// Console type values based on:
//  - YAGCD 4.2.1.1.2
//  - OSInit (GameCube ELF from Finding Nemo)
//  - OSReportInfo (Wii ELF from Rayman Raving Rabbids)
enum class ConsoleType : u32
{
  // 0x0XXXXXXX Retail units - Gamecube
  HW1 = 1,
  HW2 = 2,
  LatestProductionBoard = 3,
  Reserved = 4,

  // 0x0XXXXXXX Retail units - Wii
  PreProductionBoard0 = 0x10,    // Pre-production board 0
  PreProductionBoard1 = 0x11,    // Pre-production board 1
  PreProductionBoard2_1 = 0x12,  // Pre-production board 2-1
  PreProductionBoard2_2 = 0x20,  // Pre-production board 2-2
  RVL_Retail1 = 0x21,
  RVL_Retail2 = 0x22,
  RVL_Retail3 = 0x23,
  RVA1 = 0x100,  // Revolution Arcade

  // 0x1XXXXXXX Devkits - Gamecube
  // Emulators
  MacEmulator = 0x10000000,  // Mac Emulator
  PcEmulator = 0x10000001,   // PC Emulator

  // Embedded PowerPC series
  Arthur = 0x10000002,  // EPPC Arthur
  Minnow = 0x10000003,  // EPPC Minnow

  // Development HW
  // Version = (console_type & 0x0fffffff) - 3
  FirstDevkit = 0x10000004,
  SecondDevkit = 0x10000005,
  LatestDevkit = 0x10000006,
  ReservedDevkit = 0x10000007,

  // 0x1XXXXXXX Devkits - Wii
  RevolutionEmulator = 0x10000008,  // Revolution Emulator
  NDEV1_0 = 0x10000010,             // NDEV 1.0
  NDEV1_1 = 0x10000011,             // NDEV 1.1
  NDEV1_2 = 0x10000012,             // NDEV 1.2
  NDEV2_0 = 0x10000020,             // NDEV 2.0
  NDEV2_1 = 0x10000021,             // NDEV 2.1

  // 0x2XXXXXXX TDEV-based emulation HW
  // Version = (console_type & 0x0fffffff) - 3
  HW2TDEVSystem = 0x20000005,
  LatestTDEVSystem = 0x20000006,
  ReservedTDEVSystem = 0x20000007,
};

// These three are normally called from the Host thread. However, they can be called from any thread
// that isn't launched by the emulator core.
// takes boot
bool Init(Core::System& system, BootParameters* boot);
void Stop(Core::System& system);
void Shutdown(Core::System& system);

void DeclareAsCPUThread();
void UndeclareAsCPUThread();
void DeclareAsGPUThread();
void UndeclareAsGPUThread();

std::string StopMessage(bool main_thread, std::string_view message);

// Returns true when GetState returns Running or Paused.
bool IsRunning(Core::System& system);
// Returns true when GetState returns Starting, Running or Paused.
bool IsRunningOrStarting(Core::System& system);
// Returns true when GetState returns Uninitialized.
bool IsUninitialized(Core::System& system);

bool IsCPUThread();  // this tells us whether we are the CPU thread.

// SetState can be called from any thread.
void SetState(Core::System& system, State state, bool report_state_change = true,
              bool override_achievement_restrictions = false);
State GetState(Core::System& system);

// Logs a message meant for the user, formatted as printf does; the core draws nothing over the
// game.
void DisplayMessage(const char* format, ...) DOLPHIN_PRINTF_ATTR(1, 2);

// Runs run(context) on the CPU thread: queued for it from another, which waits until it is done.
// Never called from a thread the CPU thread waits for.
void RunOnCPUThread(Core::System& system, void (*run)(void* context), void* context);

/* The frame gate (dual core; Core.cpp): retro_run lets the CPU thread
 * run the next field; whether one is granted and not yet ended. */
void RunField(Core::System& system);
bool IsFieldInFlight();
/* The CPU thread: parks at the gate until a field is granted or it is
 * asked to stop; whether it left the gate ungranted and owes a wait. */
void FrameGateWait(Core::System& system, bool leave_for_jobs = false);
bool FrameGateOwed();
/* Any thread that asks the CPU thread to stop: wakes it from the gate. */
void FrameGateKick();

void UpdateInputGate(bool require_focus, bool require_full_focus = false);

// takes boot
void EmuThread(Core::System& system, BootParameters* boot);
void SingleCorePostRunShutdown();
extern BootParameters* g_boot_params;  // owned until the emulation thread takes it
// Starts EmuThread on its own thread (dual core); s_emu_thread is its handle.
// takes boot
bool StartEmuThread(Core::System& system, BootParameters* boot);
extern sthread_t* s_emu_thread;
extern bool s_frame_step;

}  // namespace Core
