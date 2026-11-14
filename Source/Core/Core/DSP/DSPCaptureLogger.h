// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Common
{
class PCAP;
}

namespace DSP
{
// Structured records of the DSP's internal data transfers, appended to a PCAP capture in a
// custom packet format. These are called from the DSP emulator's critical paths, only while a
// capture is open.

// An access (read or write) to a memory mapped register of the external interface (IFX); these
// are always 16 bits.
void CaptureIFXAccess(Common::PCAP* pcap, bool read, u16 address, u16 value);

// A DMA between main memory and DSP memory, either way, DRAM or IRAM; the control field is kept
// as the register had it, for whatever reads the capture. length is in bytes, not DSP words.
void CaptureDMA(Common::PCAP* pcap, u16 control, u32 gc_address, u16 dsp_address, u16 length,
                const u8* data);
}  // namespace DSP
