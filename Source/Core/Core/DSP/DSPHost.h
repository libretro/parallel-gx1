// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

// The user of the DSPCore library must supply a few functions so that the
// emulation core can access the environment it runs in. If the emulation
// core isn't used, for example in an asm/disasm tool, then most of these
// can be stubbed out.

namespace DSP
{
class DSPCore;
}

namespace DSP::Host
{
void DMAToDSP(u16* dst, u32 addr, u32 size);
void DMAFromDSP(const u16* src, u32 addr, u32 size);
bool OnThread();
void InterruptRequest();
void CodeLoaded(DSPCore& dsp, u32 addr, size_t size);
void CodeLoaded(DSPCore& dsp, const u8* ptr, size_t size);
}  // namespace DSP::Host
