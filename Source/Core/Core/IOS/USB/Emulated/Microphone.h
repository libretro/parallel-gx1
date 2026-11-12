// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include <retro_atomic.h>
#include <retro_spsc.h>
#include "Common/CommonTypes.h"

#include <libretro.h>

namespace IOS::HLE::USB
{
class MicrophoneState
{
public:
  virtual ~MicrophoneState() = default;

  virtual bool IsSampleOn() const = 0;
  virtual bool IsMuted() const = 0;
  virtual u32 GetDefaultSamplingRate() const = 0;
};

class Microphone
{
public:
  using FloatType = float;
  using SampleType = s16;
  using UnsignedSampleType = std::make_unsigned_t<SampleType>;

  explicit Microphone(const MicrophoneState& sampler);
  virtual ~Microphone();

  void Initialize();
  bool HasData(u32 sample_count) const;
  u16 ReadIntoBuffer(u8* ptr, u32 size);
  u16 GetLoudnessLevel() const;
  FloatType ComputeGain(FloatType relative_db) const;
  void SetSamplingRate(u32 sampling_rate);

  void PollRetroArchMic();

protected:
  static constexpr u32 BUFF_SIZE_SAMPLES = 32;

private:
  virtual s16 GetVolumeModifier() const = 0;
  virtual bool AreSamplesByteSwapped() const = 0;

  long DataCallback(const SampleType* input_buffer, long nframes);
  virtual bool IsMicrophoneMuted() const = 0;
  void UpdateLoudness(SampleType sample);

  void StreamInit();
  void StreamTerminate();
  void StreamStart(u32 sampling_rate);
  void StreamStop();

  virtual u32 GetStreamSize() const = 0;
  // Samples from the frontend thread (DataCallback) to the emulation thread
  // (ReadIntoBuffer). Sized in StreamStart, while nothing polls this microphone.
  retro_spsc_t m_stream{};
  bool m_stream_allocated = false;

  // TODO: Find how this level is calculated on real hardware
  retro_atomic_int_t m_loudness_level{}; /* u16 */
  struct Loudness
  {
    void Update(SampleType sample);

    SampleType GetPeak() const;
    static FloatType GetDecibel(FloatType value);
    FloatType GetAmplitude() const;
    FloatType GetAmplitudeDb() const;
    FloatType GetAbsoluteMean() const;
    FloatType GetRootMeanSquare() const;
    FloatType GetCrestFactor() const;
    static FloatType ComputeGain(FloatType db);

    void Reset();
    void LogStats();

    // Samples used to compute the loudness level (arbitrarily chosen)
    static constexpr u16 SAMPLES_NEEDED = 128;
    static_assert((SAMPLES_NEEDED % BUFF_SIZE_SAMPLES) == 0);

    static constexpr FloatType MAX_AMPLITUDE =
        UnsignedSampleType{std::numeric_limits<UnsignedSampleType>::max() / 2};
    static const FloatType DB_MIN;
    static const FloatType DB_MAX;

    u16 samples_count = 0;
    u32 absolute_sum = 0;
    FloatType square_sum = FloatType(0);
    SampleType peak_min = 0;
    SampleType peak_max = 0;
  };
  Loudness m_loudness;

  const MicrophoneState& m_sampler;

  void* m_retro_mic = nullptr;
  u32 m_current_sampling_rate = 0;
};
}  // namespace IOS::HLE::USB
