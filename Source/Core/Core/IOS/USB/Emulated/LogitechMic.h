// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Microphone.h"

namespace IOS::HLE::USB
{

class LogitechMicState final : public MicrophoneState
{
public:
  // Use atomic for members concurrently used by the data callback
  retro_atomic_int_t mute{};
  retro_atomic_int_t volume{};                                             /* u8 */
  retro_atomic_int_t sample_rate{static_cast<int>(DEFAULT_SAMPLING_RATE)}; /* u32 */

  static constexpr u32 DEFAULT_SAMPLING_RATE = 48000;

  bool IsSampleOn() const override;
  bool IsMuted() const override;
  u32 GetDefaultSamplingRate() const override;
};

class LogitechMic final : public Device
{
public:
  explicit LogitechMic(u8 index);

  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(CtrlMessage* cmd) override;
  int SubmitTransfer(BulkMessage* cmd) override;
  int SubmitTransfer(IntrMessage* cmd) override;
  int SubmitTransfer(IsoMessage* cmd) override;

private:
  // What each SubmitTransfer does with its message, which it frees afterwards
  int HandleTransfer(CtrlMessage* cmd);
  int HandleTransfer(BulkMessage* cmd);
  int HandleTransfer(IntrMessage* cmd);
  int HandleTransfer(IsoMessage* cmd);
  LogitechMicState m_sampler{};

  int GetAudioControl(CtrlMessage* cmd);
  int SetAudioControl(CtrlMessage* cmd);
  int EndpointAudioControl(CtrlMessage* cmd);

  const u16 m_vid = 0x046d;
  const u16 m_pid = 0x0a03;
  u8 m_index = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  std::unique_ptr<Microphone> m_microphone{};
};
}  // namespace IOS::HLE::USB
