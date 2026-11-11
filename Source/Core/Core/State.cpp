// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/State.h"

#include <cstdio>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/GeckoCode.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/VideoBackendBase.h"

namespace State
{
void DoState(Core::System& system, PointerWrap& p)
{
  bool is_wii = system.IsWii() || system.IsMIOS();
  const bool is_wii_currently = is_wii;
  p.Do(is_wii);
  if (is_wii != is_wii_currently)
  {
    Core::DisplayMessage(
        StringFromFormat("Cannot load a savestate created under %s mode in %s mode",
                         is_wii ? "Wii" : "GC", is_wii_currently ? "Wii" : "GC"));
    p.SetMeasureMode();
    return;
  }

  // Check to make sure the emulated memory sizes are the same as the savestate
  auto& memory = system.GetMemory();
  u32 state_mem1_size = memory.GetRamSizeReal();
  u32 state_mem2_size = memory.GetExRamSizeReal();
  p.Do(state_mem1_size);
  p.Do(state_mem2_size);
  if (state_mem1_size != memory.GetRamSizeReal() || state_mem2_size != memory.GetExRamSizeReal())
  {
    Core::DisplayMessage(StringFromFormat(
        "Memory size mismatch!\n"
        "Current | MEM1 %08X (%3uMB)    MEM2 %08X (%3uMB)\n"
        "State   | MEM1 %08X (%3uMB)    MEM2 %08X (%3uMB)",
        memory.GetRamSizeReal(), memory.GetRamSizeReal() / 0x100000U, memory.GetExRamSizeReal(),
        memory.GetExRamSizeReal() / 0x100000U, state_mem1_size, state_mem1_size / 0x100000U,
        state_mem2_size, state_mem2_size / 0x100000U));
    p.SetMeasureMode();
    return;
  }

  // The movie counters, kept for the state layout: no movie is ever recorded or played.
  {
    u64 movie_counters[4] = {};
    bool movie_polled = false;
    u64 movie_tick_count_at_last_input = 0;
    for (u64& counter : movie_counters)
      p.Do(counter);
    p.Do(movie_polled);
    p.Do(movie_tick_count_at_last_input);
  }
  p.DoMarker("Movie");

  // Begin with video backend, so that it gets a chance to clear its caches and writeback modified
  // things to RAM
  g_video_backend->DoState(p);
  p.DoMarker("video_backend");

  // The VI event goes back on the very next half-line before the event queue is saved, as one
  // event per half-line had it: a state is the same whichever build saved it.
  if (!p.IsReadMode())
    system.GetVideoInterface().PrepareForState();

  // CoreTiming needs to be restored before restoring Hardware because
  // the controller code might need to schedule an event if the controller has changed.
  system.GetCoreTiming().DoState(p);
  p.DoMarker("CoreTiming");

  // HW needs to be restored before PowerPC because the data cache might need to be flushed.
  HW::DoState(system, p);
  p.DoMarker("HW");

  system.GetPowerPC().DoState(p);
  p.DoMarker("PowerPC");

  if (system.IsWii())
    Wiimote::DoState(p);
  p.DoMarker("Wiimote");
  Gecko::DoState(p);
  p.DoMarker("Gecko");
}
}  // namespace State
