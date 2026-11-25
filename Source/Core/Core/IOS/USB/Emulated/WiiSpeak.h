// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Microphone.h"

namespace IOS::HLE::USB
{
class WiiSpeakState final : public MicrophoneState
{
public:
  // Use atomic for members concurrently used by the data callback
  retro_atomic_int_t sample_on{};
  retro_atomic_int_t mute{};
  int freq;
  int gain;
  bool ec_reset;
  bool sp_on;

  static constexpr u32 DEFAULT_SAMPLING_RATE = 16000;

  bool IsSampleOn() const override;
  bool IsMuted() const override;
  u32 GetDefaultSamplingRate() const override;
};

class WiiSpeak final : public Device
{
public:
  WiiSpeak();
  ~WiiSpeak() override;

  DeviceDescriptor GetDeviceDescriptor() const override;
  size_t GetConfigurations(ConfigDescriptor* out, size_t max) const override;
  size_t GetInterfaces(u8 config, InterfaceDescriptor* out, size_t max) const override;
  size_t GetEndpoints(u8 config, u8 iface, u8 alt, EndpointDescriptor* out,
                      size_t max) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 f) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(CtrlMessage* message) override;
  int SubmitTransfer(BulkMessage* message) override;
  int SubmitTransfer(IntrMessage* message) override;
  int SubmitTransfer(IsoMessage* message) override;

private:
  // What each SubmitTransfer does with its message, which it frees afterwards
  int HandleTransfer(CtrlMessage* cmd);
  int HandleTransfer(BulkMessage* cmd);
  int HandleTransfer(IntrMessage* cmd);
  int HandleTransfer(IsoMessage* cmd);
  WiiSpeakState m_sampler{};

  enum Registers
  {
    SAMPLER_STATE = 0,
    SAMPLER_MUTE = 0x0c,

    SAMPLER_FREQ = 2,
    FREQ_8KHZ = 0,
    FREQ_11KHZ = 1,
    FREQ_RESERVED = 2,
    FREQ_16KHZ = 3,  // default

    SAMPLER_GAIN = 4,
    GAIN_00dB = 0,
    GAIN_15dB = 1,
    GAIN_30dB = 2,
    GAIN_36dB = 3,  // default

    EC_STATE = 0x14,

    SP_STATE = 0x38,
    SP_ENABLE = 0x1010,
    SP_SIN = 0x2001,
    SP_SOUT = 0x2004,
    SP_RIN = 0x200d
  };

  void GetRegister(CtrlMessage* cmd) const;
  void SetRegister(CtrlMessage* cmd);

  const u16 m_vid = 0x057E;
  const u16 m_pid = 0x0308;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  bool init = false;
  Microphone* m_microphone = nullptr;  // owned
  const DeviceDescriptor m_device_descriptor{0x12,  0x1,    0x200,  0,   0,   0,   0x10,
                                             0x57E, 0x0308, 0x0214, 0x1, 0x2, 0x0, 0x1};
  static constexpr u8 ENDPOINT_AUDIO_IN = 0x81;
  static constexpr u8 ENDPOINT_AUDIO_OUT = 0x3;
  static constexpr u8 ENDPOINT_DATA_OUT = 0x2;
};
}  // namespace IOS::HLE::USB
