// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "AudioCommon/Mixer.h"
#include "Common/CommonTypes.h"

// The libretro audio output: owns the mixer and hands its output to the frontend.
// Implemented in DolphinLibretro/Audio.cpp.
class SoundStream final
{
public:
  static constexpr unsigned int MIN_SAMPLES = 96;
  static constexpr unsigned int MAX_SAMPLES = 1024;

  SoundStream();

  Mixer* GetMixer() const { return m_mixer.get(); }

  void Init();
  // After the DSP has pushed num_samples into the mixer
  void Update(unsigned int num_samples);
  // From the frontend's audio callback
  void ProcessCallBack();
  // On the CPU thread at each emulated field boundary
  void NewField();

private:
  void PushAudioForFrame();
  void MixAndPush(unsigned int num_samples);

  std::unique_ptr<Mixer> m_mixer;
  s16 m_buffer[MAX_SAMPLES * 2];
  unsigned int m_sample_rate;
};
