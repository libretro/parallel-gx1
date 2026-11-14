// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/BitField.h"
#include "Common/CommonTypes.h"

class PointerWrap;

namespace DSP
{
class DSPManager;
struct SDSP;

// Whose accelerator it is, which decides what its end exceptions do
enum class AcceleratorKind : u8
{
  LLE,
  AESnd,
  AXGC,
  AXWii,
};

class Accelerator
{
public:
  /* @aram: where every accelerator reads and writes, LLE's and the HLE microcodes' alike; @dsp:
   * the LLE DSP an exception is raised in */
  Accelerator(DSPManager* aram, AcceleratorKind kind, SDSP* dsp = nullptr)
  {
    Init(aram, kind, dsp);
  }
  // As made anew: its registers cleared
  void Init(DSPManager* aram, AcceleratorKind kind, SDSP* dsp = nullptr);

  u16 ReadSample(const s16* coefs);
  // Zelda ucode reads ARAM through 0xffd3.
  u16 ReadRaw();
  void WriteRaw(u16 value);

  u32 GetStartAddress() const { return m_start_address; }
  u32 GetEndAddress() const { return m_end_address; }
  u32 GetCurrentAddress() const { return m_current_address; }
  u16 GetSampleFormat() const { return m_sample_format.hex; }
  s16 GetGain() const { return m_gain; }
  s16 GetYn1() const { return m_yn1; }
  s16 GetYn2() const { return m_yn2; }
  u16 GetPredScale() const { return m_pred_scale; }
  u16 GetInput() const { return m_input; }
  void SetStartAddress(u32 address);
  void SetEndAddress(u32 address);
  void SetCurrentAddress(u32 address);
  void SetSampleFormat(u16 format);
  void SetGain(s16 gain);
  void SetYn1(s16 yn1);
  void SetYn2(s16 yn2);
  void SetPredScale(u16 pred_scale);
  void SetInput(u16 input);

  void DoState(PointerWrap& p);

  // The AX parameter block of the voice it reads for
  void* pb = nullptr;

protected:
  void OnRawReadEndException();
  void OnRawWriteEndException();
  void OnSampleReadEndException();
  u8 ReadMemory(u32 address);
  void WriteMemory(u32 address, u8 value);
  u16 GetCurrentSample();

  DSPManager* m_aram;
  SDSP* m_dsp;

  // DSP accelerator registers.
  u32 m_start_address = 0;
  u32 m_end_address = 0;
  u32 m_current_address = 0;

  enum class FormatSize : u16
  {
    Size4Bit = 0,
    Size8Bit = 1,
    Size16Bit = 2,
    SizeInvalid = 3
  };

  enum class FormatDecode : u16
  {
    ADPCM = 0,         // ADPCM reads from ARAM, ACCA increments
    MMIOPCMNoInc = 1,  // PCM Reads from ACIN, ACCA doesn't increment
    PCM = 2,           // PCM reads from ARAM, ACCA increments
    MMIOPCMInc = 3     // PCM reads from ACIN, ACCA increments
  };

  // When reading samples (at least in PCM mode), they are multiplied by the gain, then divided by
  // the value specified here
  enum class FormatGainScale : u16
  {
    GainScale2048 = 0,
    GainScale1 = 1,
    GainScale65536 = 2,
    GainScaleInvalid = 3
  };

  union SampleFormat
  {
    u16 hex;
    BitField<0, 2, FormatSize> size;
    BitField<2, 2, FormatDecode> decode;
    BitField<4, 2, FormatGainScale> gain_scale;
    BitField<6, 10, u16> unk;
  } m_sample_format{0};

  s16 m_gain = 0;
  s16 m_yn1 = 0;
  s16 m_yn2 = 0;
  u16 m_pred_scale = 0;
  u16 m_input = 0;

  // When an ACCOV is triggered, the accelerator stops reading back anything
  // and updating the current address register, unless the YN2 register is written to.
  // This is kept track of internally; this state is not exposed via any register.
  bool m_reads_stopped = false;
  AcceleratorKind m_kind;
};

namespace HLE
{
// An AX voice's sample read end: loops it or stops it. Defined with the AX microcodes, one for
// the GameCube's parameter blocks and one for the Wii's.
void AXGCSampleReadEnd(Accelerator* accelerator);
void AXWiiSampleReadEnd(Accelerator* accelerator);
}  // namespace HLE
}  // namespace DSP
