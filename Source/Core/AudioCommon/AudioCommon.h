// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "AudioCommon/Enums.h"
#include "AudioCommon/SoundStream.h"

namespace Core
{
class System;
}

namespace AudioCommon
{
void InitSoundStream(Core::System& system);
void PostInitSoundStream(Core::System& system);
void ShutdownSoundStream(Core::System& system);
DPL2Quality GetDefaultDPL2Quality();
void SetSoundStreamRunning(Core::System& system, bool running);
void SendAIBuffer(Core::System& system, const short* samples, unsigned int num_samples);
void StartAudioDump(Core::System& system);
void StopAudioDump(Core::System& system);
}  // namespace AudioCommon
