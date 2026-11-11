// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/GCMemcardRaw.h"

#include <cstddef>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <cstdio>

#include "Common/FrameClock.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/Sram.h"
#include "Core/System.h"

#define SIZE_TO_Mb (1024 * 8 * 16)
#define MC_HDR_SIZE 0xA000

MemoryCard::MemoryCard(std::string filename, ExpansionInterface::Slot card_slot, u16 size_mbits)
    : MemoryCardBase(card_slot, size_mbits), m_filename(std::move(filename))
{
  File::IOFile file(m_filename, "rb");
  if (file)
  {
    // Measure size of the existing memcard file.
    m_memory_card_size = (u32)file.GetSize();
    m_nintendo_card_id = m_memory_card_size / SIZE_TO_Mb;
    m_memcard_data = std::make_unique_for_overwrite<u8[]>(m_memory_card_size);

    INFO_LOG(EXPANSIONINTERFACE, "Reading memory card %s", m_filename.c_str());
    size_t got = 0;
    file.ReadArray(&m_memcard_data[0], m_memory_card_size, &got);
    if (got < m_memory_card_size)
      memset(&m_memcard_data[got], 0xFF, m_memory_card_size - got);
  }
  else
  {
    // Create a new 128Mb memcard
    m_nintendo_card_id = size_mbits;
    m_memory_card_size = size_mbits * SIZE_TO_Mb;

    m_memcard_data = std::make_unique<u8[]>(m_memory_card_size);

    // Fills in the first 5 blocks (MC_HDR_SIZE bytes)
    const auto& sram = Core::System::GetInstance().GetSRAM();
    const CardFlashId& flash_id = sram.settings_ex.flash_id[Memcard::SLOT_A];
    const bool shift_jis = m_filename.contains(".JAP.raw");
    const u32 rtc_bias = sram.settings.rtc_bias;
    const u32 sram_language = static_cast<u32>(sram.settings.language);
    const u64 format_time =
        Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;
    Memcard::GCMemcard::Format(&m_memcard_data[0], flash_id, size_mbits, shift_jis, rtc_bias,
                               sram_language, format_time);

    // Fills in the remaining blocks
    memset(&m_memcard_data[MC_HDR_SIZE], 0xFF, m_memory_card_size - MC_HDR_SIZE);

    INFO_LOG(EXPANSIONINTERFACE, "No memory card found. A new one was created instead.");
  }

  // Class members (including inherited ones) have now been initialized, so
  // it's safe to startup the flush thread (which reads them).
  m_flush_buffer = std::make_unique<u8[]>(m_memory_card_size);
  memcpy(&m_flush_buffer[0], &m_memcard_data[0], m_memory_card_size);
  const u32 num_blocks = (m_memory_card_size + Memcard::BLOCK_SIZE - 1) / Memcard::BLOCK_SIZE;
  m_num_blocks = num_blocks < MAX_BLOCKS ? num_blocks : MAX_BLOCKS;
  m_flush_thread = sthread_create(Common::ThreadEntry<&MemoryCard::FlushThread>, this);
}

MemoryCard::~MemoryCard()
{
  if (m_flush_thread)
  {
    m_flush_running.Clear();
    frame_clock_wake_all();

    Common::JoinThread(m_flush_thread);
  }
  for (mpsc_stack_node_t* node = mpsc_stack_drain(&m_changes); node;)
  {
    mpsc_stack_node_t* const next = node->next;
    std::free(node);
    node = next;
  }
}

void MemoryCard::PostChange(u32 offset, u32 length, const u8* data)
{
  auto* change = static_cast<Change*>(
      std::malloc(offsetof(Change, data) + (data ? length : 0) + 1));
  change->offset = offset;
  change->length = length;
  change->fill = data == nullptr;
  if (data)
    memcpy(change->data, data, length);
  mpsc_stack_push(&m_changes, &change->link);
}

void MemoryCard::ApplyChanges()
{
  for (mpsc_stack_node_t* node = mpsc_stack_reverse(mpsc_stack_drain(&m_changes)); node;)
  {
    auto* change = reinterpret_cast<Change*>(node);
    node = node->next;
    if (change->fill)
      memset(&m_flush_buffer[change->offset], 0xFF, change->length);
    else
      memcpy(&m_flush_buffer[change->offset], change->data, change->length);
    if (change->length)
    {
      const u32 last = (change->offset + change->length - 1) / Memcard::BLOCK_SIZE;
      for (u32 block = change->offset / Memcard::BLOCK_SIZE; block <= last && block < MAX_BLOCKS;
           block++)
      {
        m_flush_dirty[block >> 5] |= 1u << (block & 31);
      }
    }
    std::free(change);
  }
}

bool MemoryCard::WriteChanges(File::IOFile& file, bool whole)
{
  u32 block = 0;

  // a block stays marked until it is written
  if (whole)
  {
    if (!file.WriteBytes(&m_flush_buffer[0], m_memory_card_size))
      return false;
    std::memset(m_flush_dirty, 0, sizeof(m_flush_dirty));
    return true;
  }

  while (block < m_num_blocks)
  {
    u32 end;
    u32 word = m_flush_dirty[block >> 5] >> (block & 31);
    if (!word)
    {
      // the rest of this word is clean
      block = (block | 31) + 1;
      continue;
    }
    block += static_cast<u32>(std::countr_zero(word));
    for (end = block + 1; end < m_num_blocks && (m_flush_dirty[end >> 5] >> (end & 31)) & 1; end++)
    {
    }
    const u32 offset = block * Memcard::BLOCK_SIZE;
    const u32 end_offset = end * Memcard::BLOCK_SIZE;
    const u32 length =
        (end_offset < m_memory_card_size ? end_offset : m_memory_card_size) - offset;
    if (!file.Seek(offset, File::SeekOrigin::Begin) ||
        !file.WriteBytes(&m_flush_buffer[offset], length))
    {
      return false;
    }
    for (; block < end; block++)
      m_flush_dirty[block >> 5] &= ~(1u << (block & 31));
  }
  return true;
}

void MemoryCard::FlushThread()
{
  if (!Config::Get(Config::SESSION_SAVE_DATA_WRITABLE))
  {
    return;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "Memcard %s flushing thread",
           ExpansionInterface::SlotName(m_card_slot));
  Common::SetCurrentThreadName(thread_name);

  // About fifteen seconds of frontend frames.
  constexpr u64 FLUSH_INTERVAL_FRAMES = 15 * 60;

  while (true)
  {
    // Stopped: flush once more and exit. Otherwise check whether there is anything to flush.
    const bool do_exit = !frame_clock_wait_until(frame_clock_now() + FLUSH_INTERVAL_FRAMES,
                                                 m_flush_running.Raw());
    if (!do_exit)
    {
      const bool is_dirty = m_dirty.TestAndClear();
      if (!is_dirty)
      {
        continue;
      }
    }

    // Opening the file is purposefully done each iteration to ensure the
    // file doesn't disappear out from under us after the first check.
    // Only the blocks that changed are written over a card of the right size.
    File::IOFile file(m_filename, "r+b");
    bool whole = !file || file.GetSize() != m_memory_card_size ||
                 m_memory_card_size > MAX_BLOCKS * Memcard::BLOCK_SIZE;

    if (!file)
    {
      std::string dir;
      SplitPath(m_filename, &dir, nullptr, nullptr);
      if (!File::IsDirectory(dir))
      {
        File::CreateFullPath(dir);
      }
      file.Open(m_filename, "wb");
    }

    // Note - file may have changed above, after ctor
    if (!file)
    {
      PanicAlert(
          "Could not write memory card file %s.\n\n"
          "Are you running Dolphin from a CD/DVD, or is the save file maybe write protected?\n\n"
          "Are you receiving this after moving the emulator directory?\nIf so, then you may "
          "need to re-specify your memory card location in the options.",
          m_filename.c_str());

      // Exit the flushing thread - further flushes will be ignored unless
      // the thread is recreated.
      return;
    }

    ApplyChanges();
    if (!WriteChanges(file, whole))
      m_dirty.Set();

    if (do_exit)
      return;

    Core::DisplayMessage(m_card_slot == ExpansionInterface::Slot::A ? "Wrote to Memory Card A" :
                                                                      "Wrote to Memory Card B",
                         4000);
  }
}

void MemoryCard::MakeDirty()
{
  m_dirty.Set();
}

s32 MemoryCard::Read(u32 src_address, s32 length, u8* dest_address)
{
  if (!IsAddressInBounds(src_address, length))
  {
    PanicAlert("MemoryCard: Read called with invalid source address (%#x)", src_address);
    return -1;
  }

  memcpy(dest_address, &m_memcard_data[src_address], length);
  return length;
}

s32 MemoryCard::Write(u32 dest_address, s32 length, const u8* src_address)
{
  if (!IsAddressInBounds(dest_address, length))
  {
    PanicAlert("MemoryCard: Write called with invalid destination address (%#x)", dest_address);
    return -1;
  }

  memcpy(&m_memcard_data[dest_address], src_address, length);
  PostChange(dest_address, length, src_address);
  MakeDirty();
  return length;
}

void MemoryCard::ClearBlock(u32 address)
{
  if (address & (Memcard::BLOCK_SIZE - 1) || !IsAddressInBounds(address, Memcard::BLOCK_SIZE))
  {
    PanicAlert("MemoryCard: ClearBlock called on invalid address (%#x)", address);
    return;
  }
  memset(&m_memcard_data[address], 0xFF, Memcard::BLOCK_SIZE);
  PostChange(address, Memcard::BLOCK_SIZE, nullptr);
  MakeDirty();
}

void MemoryCard::ClearAll()
{
  memset(&m_memcard_data[0], 0xFF, m_memory_card_size);
  PostChange(0, m_memory_card_size, nullptr);
  MakeDirty();
}

void MemoryCard::DoState(PointerWrap& p)
{
  p.Do(m_card_slot);
  p.Do(m_memory_card_size);
  p.DoArray(&m_memcard_data[0], m_memory_card_size);
  // A loaded state replaces the whole card; the flush thread gets it as one change.
  if (p.IsReadMode())
    PostChange(0, m_memory_card_size, &m_memcard_data[0]);
}
