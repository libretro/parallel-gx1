// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

#include "Core/HW/Triforce/SerialDevice.h"

namespace Triforce
{

// IC card reader.
// Used by: GekitouProYakyuu, KeyOfAvalon, VirtuaStriker4[_2006]
class ICCardReader final : public SerialDevice
{
public:
  explicit ICCardReader(u8 slot_index);

  void Update() override;

  bool IsCardPresent() const;
  bool IsEjecting() const;
  bool IsReadyToInsertCard() const;

  void InsertCard();
  void EjectCard();

  u8 GetCardPresentInsertCheckCount() const { return m_card_present_insert_check_count; }

  void DoState(PointerWrap& p) override;

private:
  void CreateCards(u8 card_count);

  void SendReply(u8 command, u16 status_code, const u8* payload, std::size_t payload_size);

  static constexpr std::size_t IC_PAGE_SIZE = 8;
  static constexpr u32 IC_PAGE_COUNT = 256;

  // TODO: Maybe we're supposed to error for out-of-bound pages instead?
  static constexpr u32 IC_PAGE_INDEX_MASK = 0xff;

  class ICCard
  {
  public:
    // A card is set up in place when it is inserted.
    void Reset(std::string filename, const u8* uid);

    // Load from file if it exists, else create fresh card data.
    void Initialize();

    // IC_PAGE_SIZE bytes
    const u8* GetUID() const { return m_uid; }
    bool IsHalted() const { return m_current_state == State::Halted; }

    // The card's own bytes, lent; nullptr on error.
    const u8* ReadData(u16 page, u16 page_count, std::size_t* size);

    bool WriteData(u16 page, const u8* data, std::size_t size);

    // Returns the new value.
    u16 DecreaseUseCount(u16 page, u16 amount);

    void SetHalted() { m_current_state = State::Halted; }
    void SetIdle() { m_current_state = State::Idle; }

    void DoState(PointerWrap& p);

  private:
    void FlushData(u32 byte_offset, u32 byte_count);

    u8 m_data[IC_PAGE_SIZE * IC_PAGE_COUNT] = {};

    std::string m_filename;

    u8 m_uid[IC_PAGE_SIZE] = {};

    enum class State : u8
    {
      Idle = 0,
      Halted = 1,
    };

    // I believe "Halted" cards don't respond in "AntiCollision".
    State m_current_state = State::Idle;
  };

  // FYI: These are more like multiple RFID tags on a single card.
  // Avalon makes use of this for not yet fully understood reasons.
  // The cards stay in place; only the first m_ic_card_count are inserted.
  static constexpr std::size_t MAX_IC_CARDS = 4;
  ICCard m_ic_cards[MAX_IC_CARDS];
  std::size_t m_ic_card_count = 0;

  bool m_is_field_on = false;

  const u8 m_slot_index;

  u8 m_eject_timer{};

  // Avalon spams InsertCheck when it's waiting for a card to be removed.
  // We listen for that to automatically remove the card.
  u8 m_card_present_insert_check_count{};
};

}  // namespace Triforce
