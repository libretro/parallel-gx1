// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include <queues/mpsc_stack.h>
#include <rthreads/rthreads.h>

#include "Common/Event.h"
#include "Common/Flag.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardBase.h"

class PointerWrap;

class MemoryCard : public MemoryCardBase
{
public:
  MemoryCard(std::string filename, ExpansionInterface::Slot card_slot,
             u16 size_mbits = Memcard::MBIT_SIZE_MEMORY_CARD_2043);
  ~MemoryCard() override;
  void FlushThread();
  void MakeDirty();

  s32 Read(u32 src_address, s32 length, u8* dest_address) override;
  s32 Write(u32 dest_address, s32 length, const u8* src_address) override;
  void ClearBlock(u32 address) override;
  void ClearAll() override;
  void DoState(PointerWrap& p) override;

private:
  // A change the flush thread has yet to copy into its own buffer: length bytes at offset,
  // taken from data, or set to 0xFF when fill.
  struct Change
  {
    mpsc_stack_node_t link;  // first: the drained chain is cast back
    u32 offset;
    u32 length;
    bool fill;
    u8 data[1];
  };
  // CPU thread: hands a change to the flush thread.
  void PostChange(u32 offset, u32 length, const u8* data);
  // Flush thread: applies what was posted, oldest first.
  void ApplyChanges();

  bool IsAddressInBounds(u32 address, u32 length) const
  {
    const u64 end_address = static_cast<u64>(address) + static_cast<u64>(length);
    return end_address <= static_cast<u64>(m_memory_card_size);
  }

  std::string m_filename;
  // The card as the emulated side sees it; CPU thread only.
  std::unique_ptr<u8[]> m_memcard_data;
  // The flush thread's own copy, brought up to date from m_changes before each write.
  std::unique_ptr<u8[]> m_flush_buffer;
  mpsc_stack_t m_changes{};
  sthread_t* m_flush_thread = nullptr;
  Common::Event m_flush_trigger;
  Common::Flag m_dirty;
  u32 m_memory_card_size;
};
