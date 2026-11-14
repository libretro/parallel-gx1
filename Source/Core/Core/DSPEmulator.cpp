// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DSPEmulator.h"

#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPLLE/DSPLLE.h"

static DSP::HLE::DSPHLE* AsHLE(DSPEmulator* emulator)
{
  return static_cast<DSP::HLE::DSPHLE*>(emulator);
}

static DSP::LLE::DSPLLE* AsLLE(DSPEmulator* emulator)
{
  return static_cast<DSP::LLE::DSPLLE*>(emulator);
}

DSPEmulator* CreateDSPEmulator(Core::System& system, bool hle)
{
  if (hle)
    return new DSP::HLE::DSPHLE(system);
  return new DSP::LLE::DSPLLE();
}

void DestroyDSPEmulator(DSPEmulator* emulator)
{
  if (!emulator)
    return;
  if (emulator->IsLLE())
    delete AsLLE(emulator);
  else
    delete AsHLE(emulator);
}

#define DSP_EMULATOR_CALL(call) (IsLLE() ? AsLLE(this)->call : AsHLE(this)->call)

bool DSPEmulator::Initialize(bool wii, bool dsp_thread)
{
  return DSP_EMULATOR_CALL(Initialize(wii, dsp_thread));
}

void DSPEmulator::Shutdown()
{
  DSP_EMULATOR_CALL(Shutdown());
}

void DSPEmulator::DoState(PointerWrap& p)
{
  DSP_EMULATOR_CALL(DoState(p));
}

void DSPEmulator::PauseAndLock()
{
  DSP_EMULATOR_CALL(PauseAndLock());
}

void DSPEmulator::UnpauseAndUnlock()
{
  DSP_EMULATOR_CALL(UnpauseAndUnlock());
}

void DSPEmulator::DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value)
{
  DSP_EMULATOR_CALL(DSP_WriteMailBoxHigh(cpu_mailbox, value));
}

void DSPEmulator::DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value)
{
  DSP_EMULATOR_CALL(DSP_WriteMailBoxLow(cpu_mailbox, value));
}

u16 DSPEmulator::DSP_ReadMailBoxHigh(bool cpu_mailbox)
{
  return DSP_EMULATOR_CALL(DSP_ReadMailBoxHigh(cpu_mailbox));
}

u16 DSPEmulator::DSP_ReadMailBoxLow(bool cpu_mailbox)
{
  return DSP_EMULATOR_CALL(DSP_ReadMailBoxLow(cpu_mailbox));
}

u16 DSPEmulator::DSP_ReadControlRegister()
{
  return DSP_EMULATOR_CALL(DSP_ReadControlRegister());
}

u16 DSPEmulator::DSP_WriteControlRegister(u16 value)
{
  return DSP_EMULATOR_CALL(DSP_WriteControlRegister(value));
}

void DSPEmulator::DSP_Update(int cycles)
{
  DSP_EMULATOR_CALL(DSP_Update(cycles));
}

void DSPEmulator::DSP_StopSoundStream()
{
  DSP_EMULATOR_CALL(DSP_StopSoundStream());
}

u32 DSPEmulator::DSP_UpdateRate()
{
  return DSP_EMULATOR_CALL(DSP_UpdateRate());
}
