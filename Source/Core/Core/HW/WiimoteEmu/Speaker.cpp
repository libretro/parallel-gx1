// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteEmu/Speaker.h"

#include <cassert>


#include "AudioCommon/FieldAudio.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/ConfigManager.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace WiimoteEmu
{
// Yamaha ADPCM decoder code based on The ffmpeg Project (Copyright (s) 2001-2003)

static const s32 yamaha_difflookup[] = {1,  3,  5,  7,  9,  11,  13,  15,
                                        -1, -3, -5, -7, -9, -11, -13, -15};

static const s32 yamaha_indexscale[] = {230, 230, 230, 230, 307, 409, 512, 614,
                                        230, 230, 230, 230, 307, 409, 512, 614};

static s16 av_clip16(s32 a)
{
  if ((a + 32768) & ~65535)
    return (a >> 31) ^ 32767;
  else
    return a;
}

static s32 av_clip(s32 a, s32 amin, s32 amax)
{
  if (a < amin)
    return amin;
  else if (a > amax)
    return amax;
  else
    return a;
}

static s16 adpcm_yamaha_expand_nibble(ADPCMState& s, u8 nibble)
{
  s.predictor += (s.step * yamaha_difflookup[nibble]) / 8;
  s.predictor = av_clip16(s.predictor);
  s.step = (s.step * yamaha_indexscale[nibble]) >> 8;
  s.step = av_clip(s.step, 127, 24576);
  return s.predictor;
}

void SpeakerLogic::SpeakerData(const u8* data, int length, float speaker_pan)
{
  /* Each sample is mixed into the console's output as it is decoded, at the remote's rate:
   * nothing is collected first. */
  enum field_audio_mix mix;
  unsigned rate_dividend, volume_divisor, left, right;
  double volume;
  int i;

  if (!m_speaker_enabled || reg_data.sample_rate == 0 || length == 0)
    return;
  if (m_wiimote_index >= 4)
    return;
  mix = (enum field_audio_mix)(FIELD_AUDIO_WIIMOTE0 + m_wiimote_index);

  /* Following details from http://wiibrew.org/wiki/Wiimote#Speaker */
  if (reg_data.format == SpeakerLogic::DATA_FORMAT_PCM)
  {
    rate_dividend = 12000000;
    volume_divisor = 0xff;
  }
  else if (reg_data.format == SpeakerLogic::DATA_FORMAT_ADPCM)
  {
    rate_dividend = 6000000;
    volume_divisor = 0x7f;
  }
  else
  {
    ERROR_LOG(IOS_WIIMOTE, "Unknown speaker format %x", (unsigned)reg_data.format);
    return;
  }
  if (reg_data.volume > volume_divisor)
    volume_divisor = reg_data.volume;

  /* 0 to 255, the pan only ever turning one side down: at 0 the remote plays at its own level. */
  volume = reg_data.volume * 256.0 / volume_divisor;
  if (speaker_pan < -1.f)
    speaker_pan = -1.f;
  if (speaker_pan > 1.f)
    speaker_pan = 1.f;
  left = (unsigned)((speaker_pan > 0.f ? 1.f - speaker_pan : 1.f) * volume);
  right = (unsigned)((speaker_pan < 0.f ? 1.f + speaker_pan : 1.f) * volume);
  field_audio_mix_set_volume(mix, left > 255 ? 255 : left, right > 255 ? 255 : right);
  /* ADPCM's rate is thought to be twice the register's (3000 x2 = 6000). */
  field_audio_mix_set_rate(mix, FIELD_AUDIO_RATE_DIVIDEND /
                                    (rate_dividend / reg_data.sample_rate * 2));

  if (reg_data.format == SpeakerLogic::DATA_FORMAT_PCM)
  {
    for (i = 0; i < length; ++i)
    {
      const s16 sample = (s16)((s8)data[i] * 0x100);
      field_audio_mix_frame(mix, sample, sample);
    }
    return;
  }
  /* 4 bit Yamaha ADPCM (same as dreamcast) */
  for (i = 0; i < length; ++i)
  {
    const s16 high = adpcm_yamaha_expand_nibble(adpcm_state, (data[i] >> 4) & 0xf);
    const s16 low = adpcm_yamaha_expand_nibble(adpcm_state, data[i] & 0xf);
    field_audio_mix_frame(mix, high, high);
    field_audio_mix_frame(mix, low, low);
  }
}

void SpeakerLogic::Reset()
{
  reg_data = {};

  // Yamaha ADPCM state initialize
  adpcm_state.predictor = 0;
  adpcm_state.step = 127;
}

void SpeakerLogic::DoState(PointerWrap& p)
{
  p.Do(adpcm_state);
  p.Do(reg_data);
}

void SpeakerLogic::SetSpeakerEnabled(bool enabled)
{
  m_speaker_enabled = enabled;
}

int SpeakerLogic::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  if (I2C_ADDR != slave_addr)
    return 0;

  return RawRead(&reg_data, addr, count, data_out);
}

int SpeakerLogic::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  if (I2C_ADDR != slave_addr)
    return 0;

  if (addr == SPEAKER_DATA_OFFSET)
  {
    SpeakerData(data_in, count, m_speaker_pan_setting.GetValue() / 100);
    return count;
  }
  else
  {
    // TODO: Does writing immediately change the decoder config even when active
    // or does a write to 0x08 activate the new configuration or something?
    return RawWrite(&reg_data, addr, count, data_in);
  }
}

}  // namespace WiimoteEmu
