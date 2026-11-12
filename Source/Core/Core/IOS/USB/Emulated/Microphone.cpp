// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Emulated/Microphone.h"

#include <algorithm>
#include <cmath>
#include <ranges>

#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/System.h"

#include <libretro.h>
#include "DolphinLibretro/Common/Globals.h"

namespace IOS::HLE::USB
{
Microphone::Microphone(const MicrophoneState& sampler) : m_sampler(sampler)
{
}

Microphone::~Microphone()
{
  StreamTerminate();
  if (m_stream_allocated)
    retro_spsc_free(&m_stream);
}

void Microphone::Initialize()
{
  StreamInit();
}

void Microphone::StreamInit()
{
  StreamStart(m_sampler.GetDefaultSamplingRate());
}

void Microphone::StreamTerminate()
{
  StreamStop();
}

void Microphone::StreamStart(u32 sampling_rate)
{
  if (!Libretro::Input::g_has_microphone_support)
    return;

  if (m_retro_mic)
    StreamStop();

  retro_microphone_params_t params{};
  params.rate = sampling_rate;
  m_current_sampling_rate = sampling_rate;

  m_retro_mic = Libretro::Input::g_microphone_interface.open_mic(&params);
  if (!m_retro_mic)
  {
    WARN_LOG(IOS_USB, "Microphone: failed to open mic at %uHz", sampling_rate);
    return;
  }

  // Not being polled (StreamStop ran or it never started), so the ring is this thread's.
  if (m_stream_allocated)
    retro_spsc_free(&m_stream);
  m_stream_allocated = retro_spsc_init(&m_stream, GetStreamSize() * sizeof(SampleType));

  Libretro::Input::g_microphone_interface.set_mic_state(
    static_cast<retro_microphone_t*>(m_retro_mic), true);

  for (auto& slot : Libretro::Input::g_active_microphones)
  {
    if (!retro_atomic_load_acquire_ptr(&slot) && retro_atomic_cas_ptr(&slot, nullptr, this))
      break;
  }

  INFO_LOG(IOS_USB, "Microphone: opened mic at %uHz", sampling_rate);
}

void Microphone::StreamStop()
{
  if (!m_retro_mic)
    return;

  for (auto& slot : Libretro::Input::g_active_microphones)
    retro_atomic_cas_ptr(&slot, this, nullptr);
  // A poll of this microphone already under way finishes before it closes.
  call_gate_drain(&Libretro::Input::g_microphone_poll_gate);
  Libretro::Input::g_microphone_interface.close_mic(
      static_cast<retro_microphone_t*>(m_retro_mic));
  m_retro_mic = nullptr;
}

void Microphone::PollRetroArchMic()
{
  if (!m_retro_mic)
    return;
  if (!m_sampler.IsSampleOn() || m_sampler.IsMuted() || IsMicrophoneMuted())
    return;

  const int frames_per_call = static_cast<int>(m_current_sampling_rate / 60);
  std::vector<SampleType> buf(frames_per_call);

  const int frames = Libretro::Input::g_microphone_interface.read_mic(
    static_cast<retro_microphone_t*>(m_retro_mic), buf.data(), frames_per_call);

  if (frames > 0)
    DataCallback(buf.data(), frames);
}

long Microphone::DataCallback(const SampleType* input_buffer, long nframes)
{
  // Skip data if sampling is off or mute is on
  if (!m_sampler.IsSampleOn() || m_sampler.IsMuted())
    return nframes;

  const auto gain = ComputeGain(GetVolumeModifier());
  const bool are_samples_byte_swapped = AreSamplesByteSwapped();
  std::vector<SampleType> converted;
  converted.reserve(nframes);
  for (long i = 0; i < nframes; i++)
  {
    const SampleType le_sample = MathUtil::SaturatingCast<SampleType>(input_buffer[i] * gain);
    UpdateLoudness(le_sample);
    converted.push_back(are_samples_byte_swapped ? Common::swap16(le_sample) : le_sample);
  }

  // A full ring keeps the older samples; the guest is behind and will catch up on those.
  retro_spsc_write(&m_stream, converted.data(), converted.size() * sizeof(SampleType));

  return nframes;
}

u16 Microphone::ReadIntoBuffer(u8* ptr, u32 size)
{
  static constexpr u32 SINGLE_READ_SIZE = BUFF_SIZE_SAMPLES * sizeof(SampleType);

  u8* begin = ptr;
  if (!m_stream_allocated)
    return 0;
  for (u8* end = begin + size; ptr < end; ptr += SINGLE_READ_SIZE, size -= SINGLE_READ_SIZE)
  {
    if (size < SINGLE_READ_SIZE || retro_spsc_read_avail(&m_stream) < SINGLE_READ_SIZE)
      break;

    retro_spsc_read(&m_stream, ptr, SINGLE_READ_SIZE);
  }
  return static_cast<u16>(ptr - begin);
}

u16 Microphone::GetLoudnessLevel() const
{
  if (m_sampler.IsMuted() || IsMicrophoneMuted())
    return 0;
  return static_cast<u16>(retro_atomic_load_acquire_int(&m_loudness_level));
}

// Based on graphical cues on Monster Hunter 3, the level seems properly displayed with values
// between 0 and 0x3a00.
//
// TODO: Proper hardware testing, documentation, formulas...
void Microphone::UpdateLoudness(const SampleType sample)
{
  // Based on MH3 graphical cues, let's use a 0x4000 window
  static const u32 WINDOW = 0x4000;
  static const FloatType UNIT = (m_loudness.DB_MAX - m_loudness.DB_MIN) / WINDOW;

  m_loudness.Update(sample);

  if (m_loudness.samples_count >= m_loudness.SAMPLES_NEEDED)
  {
    const FloatType amp_db = m_loudness.GetAmplitudeDb();
    retro_atomic_store_release_int(&m_loudness_level,
                                   static_cast<u16>((amp_db - m_loudness.DB_MIN) / UNIT));

#ifdef WII_SPEAK_LOG_STATS
    m_loudness.LogStats();
#endif

    m_loudness.Reset();
  }
}

bool Microphone::HasData(u32 sample_count = BUFF_SIZE_SAMPLES) const
{
  return m_stream_allocated &&
         retro_spsc_read_avail(&m_stream) >= sample_count * sizeof(SampleType);
}

Microphone::FloatType Microphone::ComputeGain(FloatType relative_db) const
{
  return m_loudness.ComputeGain(relative_db);
}

void Microphone::SetSamplingRate(u32 sampling_rate)
{
  StreamStop();
  StreamStart(sampling_rate);
}

const Microphone::FloatType Microphone::Loudness::DB_MIN =
    20 * std::log10(FloatType(1) / MAX_AMPLITUDE);
const Microphone::FloatType Microphone::Loudness::DB_MAX = 20 * std::log10(FloatType(1));

void Microphone::Loudness::Update(const SampleType sample)
{
  ++samples_count;

  peak_min = std::min(sample, peak_min);
  peak_max = std::max(sample, peak_max);
  absolute_sum += std::abs(sample);
  square_sum += std::pow(FloatType(sample), FloatType(2));
}

Microphone::SampleType Microphone::Loudness::GetPeak() const
{
  return std::max(std::abs(peak_min), std::abs(peak_max));
}

Microphone::FloatType Microphone::Loudness::GetDecibel(FloatType value)
{
  return 20 * std::log10(value);
}

Microphone::FloatType Microphone::Loudness::GetAmplitude() const
{
  return GetPeak() / MAX_AMPLITUDE;
}

Microphone::FloatType Microphone::Loudness::GetAmplitudeDb() const
{
  return GetDecibel(GetAmplitude());
}

Microphone::FloatType Microphone::Loudness::GetAbsoluteMean() const
{
  return FloatType(absolute_sum) / samples_count;
}

Microphone::FloatType Microphone::Loudness::GetRootMeanSquare() const
{
  return std::sqrt(square_sum / samples_count);
}

Microphone::FloatType Microphone::Loudness::GetCrestFactor() const
{
  const auto rms = GetRootMeanSquare();
  if (rms == 0)
    return FloatType(0);
  return GetPeak() / rms;
}

Microphone::FloatType Microphone::Loudness::ComputeGain(FloatType db)
{
  return std::pow(FloatType(10), db / 20);
}

void Microphone::Loudness::Reset()
{
  samples_count = 0;
  absolute_sum = 0;
  square_sum = FloatType(0);
  peak_min = 0;
  peak_max = 0;
}

void Microphone::Loudness::LogStats()
{
  const auto amplitude = GetAmplitude();
  const auto amplitude_db = GetDecibel(amplitude);
  const auto rms = GetRootMeanSquare();
  const auto rms_db = GetDecibel(rms);
  const auto abs_mean = GetAbsoluteMean();
  const auto abs_mean_db = GetDecibel(abs_mean);
  const auto crest_factor = GetCrestFactor();
  const auto crest_factor_db = GetDecibel(crest_factor);

  INFO_LOG(IOS_USB,
           "Microphone loudness stats (sample count: %u/%u):\n"
           " - min=%d max=%d amplitude=%g (%g dB)\n"
           " - rms=%g (%g dB) \n"
           " - abs_mean=%g (%g dB)\n"
           " - crest_factor=%g (%g dB)",
           (unsigned)samples_count, (unsigned)SAMPLES_NEEDED, (int)peak_min, (int)peak_max,
           (double)amplitude, (double)amplitude_db, (double)rms, (double)rms_db, (double)abs_mean,
           (double)abs_mean_db, (double)crest_factor, (double)crest_factor_db);
}
}  // namespace IOS::HLE::USB
