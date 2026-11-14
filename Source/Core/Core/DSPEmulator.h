// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
class PointerWrap;

// The DSP is emulated by one of two classes derived from this, DSP::HLE::DSPHLE and
// DSP::LLE::DSPLLE. Which one is noted when it is made, and each call goes to it through a
// branch on that, not a table of virtual functions. One is made by CreateDSPEmulator and
// destroyed by DestroyDSPEmulator, which deletes it as what it is.
class DSPEmulator
{
public:
  DSPEmulator(const DSPEmulator&) = delete;
  DSPEmulator& operator=(const DSPEmulator&) = delete;

  bool IsLLE() const { return m_is_lle; }

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

protected:
  explicit DSPEmulator(bool is_lle) : m_is_lle(is_lle) {}
  ~DSPEmulator() = default;

  bool m_wii = false;

private:
  const bool m_is_lle;
};

DSPEmulator* CreateDSPEmulator(Core::System& system, bool hle);
void DestroyDSPEmulator(DSPEmulator* emulator);
