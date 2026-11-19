// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "Core/HW/HSP/HSP_Device.h"

namespace Core
{
class System;
}

namespace HSP
{
// Audio/Video data is read in chunks.
constexpr std::size_t AV_REGION_SIZE = 0x400;

// The handheld behind the player: mGBA when it is built in, an empty slot otherwise
class GBPlayerBackend;

class CHSPDevice_GBPlayer final : public IHSPDevice
{
public:
  enum class IRQ : int;

  explicit CHSPDevice_GBPlayer(Core::System& system);
  ~CHSPDevice_GBPlayer() override;

  HSPDeviceType GetDeviceType() const override { return HSPDeviceType::GBPlayer; }

  void Read(u32 address, u8* data) override;
  void Write(u32 address, const u8* data) override;

  void DoState(PointerWrap& p) override;

  void AssertIRQ(IRQ);

  // AV_REGION_SIZE entries each, lent
  u16* GetScanlineData() { return m_scanlines.data(); }
  u8* GetAudioData() { return m_audio.data(); }

private:
  void UpdateInterrupts();

  Core::System& m_system;

  std::array<u8, 0x20> m_test{};
  u8 m_control{};
  u16 m_irq{};

  // Holds four scanlines in RGB5 format.
  std::array<u16, AV_REGION_SIZE> m_scanlines{};

  // Holds PWM audio data. Entire buffer is filled and read at 4096 Hz.
  std::array<u8, AV_REGION_SIZE> m_audio{};

  std::unique_ptr<GBPlayerBackend> m_gbp;
};

}  // namespace HSP
