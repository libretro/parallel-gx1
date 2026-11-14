// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "Common/CallGate.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSPEmulator.h"

class PointerWrap;

namespace DSP::LLE
{
class DSPLLE final : public DSPEmulator
{
public:
  DSPLLE();
  ~DSPLLE();

  bool Initialize(bool wii, bool dsp_thread);
  void Shutdown();
  void DoState(PointerWrap& p);
  void PauseAndLock();
  void UnpauseAndUnlock();

  void DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value);
  void DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value);
  u16 DSP_ReadMailBoxHigh(bool cpu_mailbox);
  u16 DSP_ReadMailBoxLow(bool cpu_mailbox);
  u16 DSP_ReadControlRegister();
  u16 DSP_WriteControlRegister(u16 value);
  void DSP_Update(int cycles);
  void DSP_StopSoundStream();
  u32 DSP_UpdateRate();

private:
  static void DSPThread(void* data);

  DSPCore m_dsp_core;
  sthread_t* m_dsp_thread = nullptr;
  call_gate_t m_dsp_thread_gate{};
  bool m_is_dsp_on_thread = false;
  Common::Flag m_is_running;
  retro_atomic_int_t m_cycle_count{};

  Common::Event m_dsp_event;
  Common::Event m_ppc_event;
  bool m_request_disable_thread = false;
};
}  // namespace DSP::LLE
