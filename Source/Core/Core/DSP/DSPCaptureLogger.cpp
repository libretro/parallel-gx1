// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DSP/DSPCaptureLogger.h"

#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/PcapFile.h"

namespace DSP
{
// Definition of the packet structures stored in PCAP capture files.

const u8 IFX_ACCESS_PACKET_MAGIC = 0;
const u8 DMA_PACKET_MAGIC = 1;

#pragma pack(push, 1)
struct IFXAccessPacket
{
  u8 magic;    // IFX_ACCESS_PACKET_MAGIC
  u8 is_read;  // 0 for writes, 1 for reads.
  u16 address;
  u16 value;
};

// Followed by the bytes of the DMA.
struct DMAPacket
{
  u8 magic;         // DMA_PACKET_MAGIC
  u16 dma_control;  // Value of the DMA control register.
  u32 gc_address;   // Address in the GC RAM.
  u16 dsp_address;  // Address in the DSP RAM.
  u16 length;       // Length in bytes.
};
#pragma pack(pop)

void CaptureIFXAccess(Common::PCAP* pcap, bool read, u16 address, u16 value)
{
  IFXAccessPacket pkt;
  pkt.magic = IFX_ACCESS_PACKET_MAGIC;
  pkt.is_read = read ? 1 : 0;
  pkt.address = address;
  pkt.value = value;
  pcap->AddPacket(reinterpret_cast<const u8*>(&pkt), sizeof(pkt));
}

void CaptureDMA(Common::PCAP* pcap, u16 control, u32 gc_address, u16 dsp_address, u16 length,
                const u8* data)
{
  // A DMA is at most 64 KiB: the packet is put together in a static buffer
  static u8 buffer[sizeof(DMAPacket) + 0x10000];

  DMAPacket* pkt = reinterpret_cast<DMAPacket*>(&buffer[0]);
  pkt->magic = DMA_PACKET_MAGIC;
  pkt->dma_control = control;
  pkt->gc_address = gc_address;
  pkt->dsp_address = dsp_address;
  pkt->length = length;
  memcpy(&buffer[sizeof(DMAPacket)], data, length);

  pcap->AddPacket(buffer, sizeof(DMAPacket) + length);
}
}  // namespace DSP
