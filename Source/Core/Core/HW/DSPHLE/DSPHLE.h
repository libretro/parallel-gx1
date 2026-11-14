// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/MailHandler.h"

namespace Core
{
class System;
}
class PointerWrap;

namespace DSP::HLE
{
class UCodeInterface;

class DSPHLE final : public DSPEmulator
{
public:
  explicit DSPHLE(Core::System& system);
  DSPHLE(const DSPHLE& other) = delete;
  DSPHLE(DSPHLE&& other) = delete;
  DSPHLE& operator=(const DSPHLE& other) = delete;
  DSPHLE& operator=(DSPHLE&& other) = delete;
  ~DSPHLE();

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

  CMailHandler& AccessMailHandler() { return m_mail_handler; }
  void SetUCode(u32 crc);
  void SwapUCode(u32 crc);

  Core::System& GetSystem() const { return m_system; }

private:
  void SendMailToDSP(u32 mail);

  // Fake mailbox utility
  struct DSPState
  {
    u32 cpu_mailbox;
    u32 dsp_mailbox;

    void Reset()
    {
      cpu_mailbox = 0x00000000;
      dsp_mailbox = 0x00000000;
    }

    DSPState() { Reset(); }
  };
  DSPState m_dsp_state;

  // At most two ucodes live at once, the running one and the one swapped out to be resumed;
  // each is built in one of two slots allocated with the DSPHLE, so switching ucodes in game
  // allocates nothing.
  UCodeInterface* BuildUCode(u32 crc);
  void DestroyUCode(UCodeInterface*& ucode);

  u8* m_ucode_storage = nullptr;
  size_t m_ucode_slot_size = 0;
  UCodeInterface* m_ucode = nullptr;
  UCodeInterface* m_last_ucode = nullptr;

  DSP::UDSPControl m_dsp_control;
  u64 m_control_reg_init_code_clear_time = 0;
  CMailHandler m_mail_handler;

  Core::System& m_system;
};
}  // namespace DSP::HLE
