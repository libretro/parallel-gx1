// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace Triforce
{

class ICCardReader;
class IOAdapter;

// Triforce GPIO peripherals connect to JVS-IO and eachother in game specific ways.
// This class hopes to handle those customizable connections.
class IOPorts
{
public:
  void Update();

  // STATUS_SWITCH_BYTE_COUNT bytes
  u8* GetStatusSwitches() { return m_status_switches; }
  const u8* GetStatusSwitches() const { return m_status_switches; }

  u8* GetSystemInputs() { return &m_system_inputs; }
  const u8* GetSystemInputs() const { return &m_system_inputs; }

  // SWITCH_INPUT_BYTES_PER_PLAYER bytes
  u8* GetSwitchInputs(u32 player_index);
  const u8* GetSwitchInputs(u32 player_index) const;

  // ANALOG_INPUT_COUNT values
  u16* GetAnalogInputs() { return m_analog_inputs; }
  const u16* GetAnalogInputs() const { return m_analog_inputs; }

  // GENERIC_OUTPUT_BYTE_COUNT bytes
  const u8* GetGenericOutputs() const { return m_generic_outputs; }

  void SetGenericOutputs(const u8* bytes, std::size_t size);
  void ResetGenericOutputs();

  static constexpr std::size_t PLAYER_COUNT = 2;
  static constexpr std::size_t COIN_SLOT_COUNT = 2;

  // COIN_SLOT_COUNT flags
  bool* GetCoinInputs() { return m_coin_inputs; }
  const bool* GetCoinInputs() const { return m_coin_inputs; }

  static constexpr std::size_t STATUS_SWITCH_BYTE_COUNT = 2;
  static constexpr std::size_t SWITCH_INPUT_BYTES_PER_PLAYER = 2;
  static constexpr std::size_t ANALOG_INPUT_COUNT = 8;
  static constexpr std::size_t GENERIC_OUTPUT_BYTE_COUNT = 4;

  void AddIOAdapter(std::unique_ptr<IOAdapter> adapter);

  void DoState(PointerWrap& p);

  static constexpr u16 NEUTRAL_ANALOG_VALUE = 0x8000;

private:
  std::vector<std::unique_ptr<IOAdapter>> m_io_adapters;

  u8 m_status_switches[STATUS_SWITCH_BYTE_COUNT] = {0xff, 0xff};

  // The first byte in JVSIO switch input data.
  u8 m_system_inputs = 0x00;

  u8 m_switch_inputs[PLAYER_COUNT * SWITCH_INPUT_BYTES_PER_PLAYER] = {};

  u16 m_analog_inputs[ANALOG_INPUT_COUNT] = {};

  u8 m_generic_outputs[GENERIC_OUTPUT_BYTE_COUNT] = {};

  bool m_coin_inputs[COIN_SLOT_COUNT] = {};
};

class IOAdapter
{
  friend IOPorts;

public:
  IOAdapter();
  virtual ~IOAdapter();

  IOAdapter(const IOAdapter&) = delete;
  IOAdapter& operator=(const IOAdapter&) = delete;
  IOAdapter(IOAdapter&&) = delete;
  IOAdapter& operator=(IOAdapter&&) = delete;

  virtual void Update();

  virtual void DoState(PointerWrap& p);

protected:
  IOPorts* GetIOPorts() { return m_io_ports; }

  // Invoked with newly set and cleared bits.
  virtual void HandleGenericOutputsChanged(const u8* bits_set, const u8* bits_cleared);

private:
  void SetIOPorts(IOPorts* io_ports) { m_io_ports = io_ports; }

  IOPorts* m_io_ports{};
};

class Common_IOAdapter final : public IOAdapter
{
public:
  explicit Common_IOAdapter(Core::System& system) : m_system{system} {}

  void Update() override;

private:
  Core::System& m_system;
};

class VirtuaStriker3_IOAdapter final : public IOAdapter
{
public:
  void Update() override;
};

// Common functionality for both VirtuaStriker4 and VirtuaStriker4_2006.
class VirtuaStriker4Common_IOAdapter final : public IOAdapter
{
public:
  VirtuaStriker4Common_IOAdapter(ICCardReader* card_reader_a, ICCardReader* card_reader_b)
      : m_card_readers{card_reader_a, card_reader_b}
  {
  }

  void Update() override;

protected:
  static constexpr u8 SLOT_A_LOCK_BIT = 0x40;
  static constexpr u8 SLOT_B_LOCK_BIT = 0x10;

  void HandleGenericOutputsChanged(const u8* bits_set, const u8* bits_cleared) override;

private:
  ICCardReader* const m_card_readers[IOPorts::PLAYER_COUNT];
};

class VirtuaStriker4_2006_IOAdapter final : public IOAdapter
{
public:
  VirtuaStriker4_2006_IOAdapter(ICCardReader* card_reader_a, ICCardReader* card_reader_b)
      : m_card_readers{card_reader_a, card_reader_b}
  {
  }

protected:
  void HandleGenericOutputsChanged(const u8* bits_set, const u8* bits_cleared) override;

private:
  static constexpr u8 SLOT_A_EJECT_BIT = 0x80;
  static constexpr u8 SLOT_B_EJECT_BIT = 0x20;

  ICCardReader* const m_card_readers[IOPorts::PLAYER_COUNT];
};

class GekitouProYakyuu_IOAdapter final : public IOAdapter
{
public:
  GekitouProYakyuu_IOAdapter(ICCardReader* card_reader_a, ICCardReader* card_reader_b)
      : m_card_readers{card_reader_a, card_reader_b}
  {
  }

  void Update() override;

protected:
  void HandleGenericOutputsChanged(const u8* bits_set, const u8* bits_cleared) override;

private:
  static constexpr u8 SLOT_A_LOCK_BIT = 0x40;
  static constexpr u8 SLOT_B_LOCK_BIT = 0x10;

  ICCardReader* const m_card_readers[IOPorts::PLAYER_COUNT];
};

class KeyOfAvalon_IOAdapter final : public IOAdapter
{
public:
  explicit KeyOfAvalon_IOAdapter(ICCardReader* card_reader) : m_card_reader{card_reader} {}

  void Update() override;

private:
  ICCardReader* const m_card_reader;
};

}  // namespace Triforce
