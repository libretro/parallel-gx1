// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/AudioCommon.h"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "AudioCommon/Mixer.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/TimeUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"

namespace AudioCommon
{
void InitSoundStream(Core::System& system)
{
  auto sound_stream = std::make_unique<SoundStream>();
  sound_stream->Init();
  system.SetSoundStream(std::move(sound_stream));
}

void PostInitSoundStream(Core::System& system)
{
  // This needs to be called after AudioInterface::Init and SerialInterface::Init (for GBA devices)
  // where input sample rates are set
  SetSoundStreamRunning(system, true);

  if (Config::Get(Config::MAIN_DUMP_AUDIO) && !system.IsAudioDumpStarted())
    StartAudioDump(system);
}

void ShutdownSoundStream(Core::System& system)
{
  INFO_LOG_FMT(AUDIO, "Shutting down sound stream");

  if (Config::Get(Config::MAIN_DUMP_AUDIO) && system.IsAudioDumpStarted())
    StopAudioDump(system);

  SetSoundStreamRunning(system, false);
  system.SetSoundStream(nullptr);

  INFO_LOG_FMT(AUDIO, "Done shutting down sound stream");
}

DPL2Quality GetDefaultDPL2Quality()
{
  return DPL2Quality::High;
}

void SetSoundStreamRunning(Core::System& system, bool running)
{
  if (system.GetSoundStream() && system.IsSoundStreamRunning() != running)
    system.SetSoundStreamRunning(running);
}

void SendAIBuffer(Core::System& system, const short* samples, unsigned int num_samples)
{
  SoundStream* const sound_stream = system.GetSoundStream();

  if (!sound_stream)
    return;

  if (Config::Get(Config::MAIN_DUMP_AUDIO) && !system.IsAudioDumpStarted())
    StartAudioDump(system);
  else if (!Config::Get(Config::MAIN_DUMP_AUDIO) && system.IsAudioDumpStarted())
    StopAudioDump(system);

  if (samples)
  {
    sound_stream->GetMixer()->PushSamples(samples, num_samples);
    sound_stream->Update(num_samples);
  }
}

void StartAudioDump(Core::System& system)
{
  const SoundStream* const sound_stream = system.GetSoundStream();

  const std::time_t start_time = std::time(nullptr);

  std::string path_prefix = File::GetUserPath(D_DUMPAUDIO_IDX) + SConfig::GetInstance().GetGameID();

  const auto local_time = Common::LocalTime(start_time);
  if (!local_time)
    return;

  std::string base_name = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", path_prefix, *local_time);

  const std::string audio_file_name_dtk = fmt::format("{}_dtkdump.wav", base_name);
  const std::string audio_file_name_dsp = fmt::format("{}_dspdump.wav", base_name);
  File::CreateFullPath(audio_file_name_dtk);
  File::CreateFullPath(audio_file_name_dsp);
  sound_stream->GetMixer()->StartLogDTKAudio(audio_file_name_dtk);
  sound_stream->GetMixer()->StartLogDSPAudio(audio_file_name_dsp);
  system.SetAudioDumpStarted(true);
}

void StopAudioDump(Core::System& system)
{
  const SoundStream* const sound_stream = system.GetSoundStream();

  if (!sound_stream)
    return;
  sound_stream->GetMixer()->StopLogDTKAudio();
  sound_stream->GetMixer()->StopLogDSPAudio();
  system.SetAudioDumpStarted(false);
}
}  // namespace AudioCommon
