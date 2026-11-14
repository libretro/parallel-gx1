// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/DSPHLE/DSPHLE.h"

#include <new>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace DSP::HLE
{
DSPHLE::DSPHLE(Core::System& system) : m_mail_handler(system.GetDSP()), m_system(system)
{
  m_ucode_slot_size = UCodeStorageSize();
  m_ucode_storage = static_cast<u8*>(::operator new(2 * m_ucode_slot_size));
}

DSPHLE::~DSPHLE()
{
  DestroyUCode(m_ucode);
  DestroyUCode(m_last_ucode);
  ::operator delete(m_ucode_storage);
}

// in the slot neither live ucode is in
UCodeInterface* DSPHLE::BuildUCode(u32 crc)
{
  u8* slot = m_ucode_storage;
  if (reinterpret_cast<u8*>(m_ucode) == slot || reinterpret_cast<u8*>(m_last_ucode) == slot)
    slot += m_ucode_slot_size;
  return UCodeFactory(slot, crc, this, m_wii);
}

void DSPHLE::DestroyUCode(UCodeInterface*& ucode)
{
  if (ucode)
    ucode->~UCodeInterface();
  ucode = nullptr;
}

bool DSPHLE::Initialize(bool wii, bool dsp_thread)
{
  m_wii = wii;
  DestroyUCode(m_ucode);
  DestroyUCode(m_last_ucode);

  SetUCode(UCODE_ROM);

  m_dsp_control.Hex = 0;
  m_dsp_control.DSPHalt = 1;
  m_dsp_control.DSPInit = 1;
  m_mail_handler.SetHalted(m_dsp_control.DSPHalt);

  m_dsp_state.Reset();

  return true;
}

void DSPHLE::DSP_StopSoundStream()
{
}

void DSPHLE::Shutdown()
{
  DestroyUCode(m_ucode);
}

void DSPHLE::DSP_Update(int cycles)
{
  if (m_ucode != nullptr)
    m_ucode->Update();
}

u32 DSPHLE::DSP_UpdateRate()
{
  // AX HLE uses 3ms (Wii) or 5ms (GC) timing period
  // But to be sure, just update the HLE every ms.
  return m_system.GetSystemTimers().GetTicksPerSecond() / 1000;
}

void DSPHLE::SendMailToDSP(u32 mail)
{
  if (m_ucode != nullptr)
  {
    DEBUG_LOG(DSP_MAIL, "CPU writes %#010x", mail);
    m_ucode->HandleMail(mail);
  }
}

void DSPHLE::SetUCode(u32 crc)
{
  m_mail_handler.ClearPending();
  DestroyUCode(m_ucode);
  m_ucode = BuildUCode(crc);
  m_ucode->Initialize();
}

// TODO do it better?
// Assumes that every odd call to this func is by the persistent ucode.
// Even callers are deleted.
void DSPHLE::SwapUCode(u32 crc)
{
  m_mail_handler.ClearPending();

  if (m_last_ucode && UCodeInterface::GetCRC(m_last_ucode) == crc)
  {
    DestroyUCode(m_ucode);
    m_ucode = m_last_ucode;
    m_last_ucode = nullptr;
  }
  else
  {
    if (!m_last_ucode)
    {
      m_last_ucode = m_ucode;
      m_ucode = nullptr;
    }
    DestroyUCode(m_ucode);
    m_ucode = BuildUCode(crc);
    m_ucode->Initialize();
  }
}

void DSPHLE::DoState(PointerWrap& p)
{
  bool is_hle = true;
  p.Do(is_hle);
  if (!is_hle && p.IsReadMode())
  {
    Core::DisplayMessage("State is incompatible with current DSP engine. Aborting load state.");
    p.SetVerifyMode();
    return;
  }

  p.Do(m_dsp_control);
  p.Do(m_control_reg_init_code_clear_time);
  p.Do(m_dsp_state);

  int ucode_crc = UCodeInterface::GetCRC(m_ucode);
  int ucode_crc_before_load = ucode_crc;
  int last_ucode_crc = UCodeInterface::GetCRC(m_last_ucode);
  int last_ucode_crc_before_load = last_ucode_crc;

  p.Do(ucode_crc);
  p.Do(last_ucode_crc);

  // if a different type of ucode was being used when the savestate was created,
  // we have to reconstruct the old type of ucode so that we have a valid thing to call DoState on.
  const bool same_ucode = ucode_crc == ucode_crc_before_load;
  const bool same_last_ucode = last_ucode_crc == last_ucode_crc_before_load;
  // a ucode replaced is destroyed first, its slot then free for the one built
  if (!same_ucode)
    DestroyUCode(m_ucode);
  if (!same_last_ucode)
    DestroyUCode(m_last_ucode);
  if (!same_ucode)
    m_ucode = BuildUCode(ucode_crc);
  if (!same_last_ucode)
    m_last_ucode = BuildUCode(last_ucode_crc);

  if (m_ucode)
    m_ucode->DoState(p);
  if (m_last_ucode)
    m_last_ucode->DoState(p);

  m_mail_handler.DoState(p);
}

// Mailbox functions
u16 DSPHLE::DSP_ReadMailBoxHigh(bool cpu_mailbox)
{
  if (cpu_mailbox)
  {
    return (m_dsp_state.cpu_mailbox >> 16) & 0xFFFF;
  }
  else
  {
    return AccessMailHandler().ReadDSPMailboxHigh();
  }
}

u16 DSPHLE::DSP_ReadMailBoxLow(bool cpu_mailbox)
{
  if (cpu_mailbox)
  {
    return m_dsp_state.cpu_mailbox & 0xFFFF;
  }
  else
  {
    return AccessMailHandler().ReadDSPMailboxLow();
  }
}

void DSPHLE::DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value)
{
  if (cpu_mailbox)
  {
    m_dsp_state.cpu_mailbox = (m_dsp_state.cpu_mailbox & 0xFFFF) | (value << 16);
  }
  else
  {
    PanicAlert("CPU can't write %08x to DSP mailbox", (unsigned)value);
  }
}

void DSPHLE::DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value)
{
  if (cpu_mailbox)
  {
    m_dsp_state.cpu_mailbox = (m_dsp_state.cpu_mailbox & 0xFFFF0000) | value;
    SendMailToDSP(m_dsp_state.cpu_mailbox);
    // Mail sent so clear MSB to show that it is progressed
    m_dsp_state.cpu_mailbox &= 0x7FFFFFFF;
  }
  else
  {
    PanicAlert("CPU can't write %08x to DSP mailbox", (unsigned)value);
  }
}

// Other DSP functions
u16 DSPHLE::DSP_WriteControlRegister(u16 value)
{
  DSP::UDSPControl temp(value);

  if (m_dsp_control.DSPHalt != temp.DSPHalt)
  {
    INFO_LOG(DSPHLE, "DSP_CONTROL halt bit changed: %04x -> %04x", (unsigned)m_dsp_control.Hex,
             (unsigned)value);
    m_mail_handler.SetHalted(temp.DSPHalt);
  }

  if (temp.DSPReset)
  {
    SetUCode(UCODE_ROM);
    temp.DSPReset = 0;
  }

  // init - unclear if writing DSPInitCode does something. Clearing DSPInit immediately sets
  // DSPInitCode, which gets unset a bit later...
  if ((m_dsp_control.DSPInit != 0) && (temp.DSPInit == 0))
  {
    // Copy 1024(?) bytes of uCode from main memory 0x81000000 (or is it ARAM 00000000?)
    // to IMEM 0000 and jump to that code
    // TODO: Determine exactly how this initialization works
    // We could hash the input data, but this is only used for initialization purposes on licensed
    // games and by devkitpro, so there's no real point in doing so.
    // Datel has similar logic to retail games, but they clear bit 0x80 (DSP) instead of bit 0x800
    // (DSPInit) so they end up not using the init uCode.
    SetUCode(UCODE_INIT_AUDIO_SYSTEM);
    temp.DSPInitCode = 1;
    // Number obtained from real hardware on a Wii, but it's not perfectly consistent
    m_control_reg_init_code_clear_time = m_system.GetSystemTimers().GetFakeTimeBase() + 130;
  }

  m_dsp_control.Hex = temp.Hex;
  return m_dsp_control.Hex;
}

u16 DSPHLE::DSP_ReadControlRegister()
{
  if (m_dsp_control.DSPInitCode != 0)
  {
    if (m_system.GetSystemTimers().GetFakeTimeBase() >= m_control_reg_init_code_clear_time)
      m_dsp_control.DSPInitCode = 0;
    else
      m_system.GetCoreTiming().ForceExceptionCheck(50);  // Keep checking
  }
  return m_dsp_control.Hex;
}

void DSPHLE::PauseAndLock()
{
}

void DSPHLE::UnpauseAndUnlock()
{
}
}  // namespace DSP::HLE
