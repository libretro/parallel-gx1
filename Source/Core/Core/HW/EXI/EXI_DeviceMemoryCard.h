// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>

#include "Core/HW/EXI/EXI_Device.h"

class MemoryCardBase;
class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
class CoreTimingManager;
}
namespace Memcard
{
struct HeaderData;
}

namespace ExpansionInterface
{
enum class Slot : int;

class CEXIMemoryCard final : public IEXIDevice
{
  // the base branches to its calls, private ones included
  friend class IEXIDevice;

public:
  CEXIMemoryCard(Core::System& system, Slot slot, bool gci_folder,
                 const Memcard::HeaderData& header_data);
  ~CEXIMemoryCard();
  void SetCS(int cs);
  bool IsInterruptSet();
  bool UseDelayedTransferCompletion() const;
  bool IsPresent() const;
  void DoState(PointerWrap& p);
  void DMARead(u32 addr, u32 size);
  void DMAWrite(u32 addr, u32 size);

  // CoreTiming events need to be registered during boot since CoreTiming is DoState()-ed
  // before ExpansionInterface so we'll lose the save stated events if the callbacks are
  // not already registered first.
  static void Init(CoreTiming::CoreTimingManager& core_timing);
  static void Shutdown();

  // The folder in path; true when a raw card is to be migrated into it on first use
  static bool GetGCIFolderPath(Slot card_slot, std::string* path);

private:
  void SetupGciFolder(const Memcard::HeaderData& header_data);
  void SetupRawMemcard(u16 size_mb);
  // The memory card in the event's slot, or nullptr when the slot holds something else.
  static CEXIMemoryCard* EventCompleteFindInstance(Core::System& system, u64 userdata);

  // Scheduled when a command that required delayed end signaling is done.
  static void CmdDoneCallback(Core::System& system, u64 userdata, s64 cyclesLate);

  // Scheduled when memory card is done transferring data
  static void TransferCompleteCallback(Core::System& system, u64 userdata, s64 cyclesLate);

  // Signals that the command that was previously executed is now done.
  void CmdDone();

  // Signals that the transfer that was previously executed is now done.
  void TransferComplete();

  // Variant of CmdDone which schedules an event later in the future to complete the command.
  void CmdDoneLater(u64 cycles);

  enum class Command
  {
    NintendoID = 0x00,
    ReadArray = 0x52,
    ArrayToBuffer = 0x53,
    SetInterrupt = 0x81,
    WriteBuffer = 0x82,
    ReadStatus = 0x83,
    ReadID = 0x85,
    ReadErrorBuffer = 0x86,
    WakeUp = 0x87,
    Sleep = 0x88,
    ClearStatus = 0x89,
    SectorErase = 0xF1,
    PageProgram = 0xF2,
    ExtraByteProgram = 0xF3,
    ChipErase = 0xF4,
  };

  Slot m_card_slot;
  //! memory card state

  // STATE_TO_SAVE
  int m_interrupt_switch;
  bool m_interrupt_set;
  Command m_command;
  int m_status;
  u32 m_position;
  u8 m_programming_buffer[128];
  //! memory card parameters
  unsigned int m_card_id;
  unsigned int m_address;
  u32 m_memory_card_size;
  // the card, raw or a GCI folder, made with the device and deleted with it
  MemoryCardBase* m_memory_card = nullptr;

protected:
  void TransferByte(u8& byte);
};
}  // namespace ExpansionInterface
