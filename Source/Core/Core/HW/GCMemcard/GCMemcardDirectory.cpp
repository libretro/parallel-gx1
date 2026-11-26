// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/GCMemcardDirectory.h"
#include "Core/HW/EXI/EXI.h"

#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <retro_miscellaneous.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <cstdio>

#include "Common/FrameClock.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include <compat/strl.h>
#include <encodings/shiftjis.h>
#include <streams/file_stream.h>
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "Core/HW/Sram.h"

static const char* MC_HDR = "MC_SYSTEM_AREA";

// Writes the escaped "maker-game-file.gci" name of a save to out, each part decoded from the
// card's encoding and cut at its first NUL; returns false when it had to be cut short.
static bool GenerateDefaultGCIFilename(const Memcard::DEntry& entry,
                                       bool card_encoding_is_shift_jis, char* out, size_t out_size)
{
  size_t (*const decode)(char*, size_t, const char*, size_t) =
      card_encoding_is_shift_jis ? sjis_to_utf8 : cp1252_to_utf8;
  char makercode[3 * sizeof(entry.m_makercode) + 1];
  char gamecode[3 * sizeof(entry.m_gamecode) + 1];
  char filename[3 * sizeof(entry.m_filename) + 1];
  char name[sizeof(makercode) + sizeof(gamecode) + sizeof(filename) + 8];

  // A part's decoding ends at its terminator or at a NUL it decoded, whichever is first.
  decode(makercode, sizeof(makercode), reinterpret_cast<const char*>(entry.m_makercode),
         ARRAY_SIZE(entry.m_makercode));
  decode(gamecode, sizeof(gamecode), reinterpret_cast<const char*>(entry.m_gamecode),
         ARRAY_SIZE(entry.m_gamecode));
  decode(filename, sizeof(filename), reinterpret_cast<const char*>(entry.m_filename),
         ARRAY_SIZE(entry.m_filename));
  snprintf(name, sizeof(name), "%s-%s-%s.gci", makercode, gamecode, filename);
  return Common::EscapeFileNameTo(name, std::strlen(name), out, out_size);
}

/* A string as a std::string was: a u32 length, then the characters */
static void DoString(PointerWrap& p, char* str, size_t size)
{
  u32 length = static_cast<u32>(strlen(str));
  p.Do(length);
  if (length >= size)
  {
    p.SetMeasureMode();
    return;
  }
  p.DoArray(str, length);
  str[length] = '\0';
}

void GCMemcardDirectory::ResetSave(u32 index)
{
  Memcard::GCIFile& save = m_saves[index];
  save.m_gci_header = Memcard::DEntry();
  save.m_num_save_blocks = 0;
  save.m_num_used_blocks = 0;
  save.m_dirty = false;
  save.m_filename[0] = '\0';
}

bool GCMemcardDirectory::LoadGCI()
{
  Memcard::GCIFile& gci = m_saves[m_num_saves];
  u32 i;

  // check if any already loaded file has the same internal name as the new file
  for (i = 0; i < m_num_saves; i++)
  {
    if (HasSameIdentity(gci.m_gci_header, m_saves[i].m_gci_header))
    {
      ERROR_LOG(EXPANSIONINTERFACE,
                "%s\nwas not loaded because it has the same internal filename as previously "
                "loaded save\n%s",
                gci.m_filename, m_saves[i].m_filename);
      return false;
    }
  }

  // check if this file has a valid block size
  // 2043 is the largest number of free blocks on a memory card
  // in reality, there are not likely any valid gci files > 251 blocks
  const u16 num_blocks = gci.m_gci_header.m_block_count;
  if (num_blocks > 2043)
  {
    ERROR_LOG(EXPANSIONINTERFACE,
              "%s\nwas not loaded because it is an invalid GCI.\nNumber of blocks claimed to be %u",
              gci.m_filename, (unsigned)num_blocks);
    return false;
  }

  // reserve storage for the save file in the BAT, where its data is read to
  const u16 last_allocated_block = m_bat1.m_last_allocated_block;
  const u16 free_blocks = m_bat1.m_free_blocks;
  const u16 first_block = m_bat1.AssignBlocksContiguous(num_blocks);
  if (first_block == 0xFFFF)
  {
    ERROR_LOG(
        EXPANSIONINTERFACE,
        "%s\nwas not loaded because there are not enough free blocks on the virtual memory card",
        gci.m_filename);
    return false;
  }
  gci.m_gci_header.m_first_block = first_block;
  SetUsedBlocks(m_num_saves);

  if (!gci.LoadSaveBlocks(m_blocks))
  {
    ERROR_LOG(EXPANSIONINTERFACE, "Failed to load data of %s", gci.m_filename);
    // the blocks are free again
    for (i = 0; i < num_blocks; i++)
      m_bat1.m_map[first_block + i - Memcard::MC_FST_BLOCKS] = 0;
    m_bat1.m_last_allocated_block = last_allocated_block;
    m_bat1.m_free_blocks = free_blocks;
    m_bat1.FixChecksums();
    return false;
  }

  if (gci.HasCopyProtection() && first_block + num_blocks <= Memcard::GCI_MAX_BLOCKS)
  {
    Memcard::GCMemcard::PSO_MakeSaveGameValid(m_hdr, gci.m_gci_header, m_blocks + first_block,
                                              num_blocks);
    Memcard::GCMemcard::FZEROGX_MakeSaveGameValid(m_hdr, gci.m_gci_header, m_blocks + first_block,
                                                  num_blocks);
  }

  // actually load save file into memory card
  m_dir1.Replace(gci.m_gci_header, m_num_saves);
  m_num_saves++;

  return true;
}

GCMemcardDirectory::GCMemcardDirectory(std::string directory, ExpansionInterface::Slot slot,
                                       const Memcard::HeaderData& header_data, u32 game_id)
    : MemoryCardBase(slot, header_data.m_size_mb), m_game_id(game_id), m_last_block(-1),
      m_hdr(header_data), m_bat1(header_data.m_size_mb)
{
  strlcpy(m_save_directory, directory.c_str(), sizeof(m_save_directory));
  m_blocks =
      static_cast<Memcard::GCMBlock*>(std::calloc(Memcard::GCI_MAX_BLOCKS, Memcard::BLOCK_SIZE));
  if (!m_blocks)
  {
    PanicAlert("Out of memory for the GCI folder's memory card");
    return;
  }

  // Use existing header data if available
  {
    File::IOFile((directory + MC_HDR), "rb").ReadBytes(&m_hdr, Memcard::BLOCK_SIZE);
  }

  const bool current_game_only = Config::Get(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY);
  path_list_t filenames;
  path_list_init(&filenames);
  Common::DoFileSearch(directory.c_str(), ".gci", false, &filenames);

  // leave about 10% of free space on the card if possible
  const int total_blocks = Memcard::MbitToFreeBlocks(m_hdr.m_data.m_size_mb);
  const int reserved_blocks = total_blocks / 10;

  // the files for the current game, which we should definitely load, then the files for other
  // games, which we don't care too much about
  size_t failed_loads_current_game = 0;
  for (int pass = 0; pass < (current_game_only ? 1 : 2); pass++)
  {
    for (unsigned i = 0; i < filenames.count; i++)
    {
      const char* const filename = path_list_get(&filenames, i);
      // leave some free file entries for new saves that might be created
      if (m_num_saves >= Memcard::DIRLEN || (pass == 1 && m_num_saves > 112))
        break;

      Memcard::GCIFile& gci = m_saves[m_num_saves];
      ResetSave(m_num_saves);
      strlcpy(gci.m_filename, filename, sizeof(gci.m_filename));
      if (!gci.LoadHeader())
      {
        if (pass == 0)
          ERROR_LOG(EXPANSIONINTERFACE, "Failed to load header of %s", filename);
        continue;
      }

      const bool current_game = m_game_id == Common::swap32(gci.m_gci_header.m_gamecode);
      if (current_game != (pass == 0))
        continue;

      if (pass == 0)
      {
        if (!LoadGCI())
        {
          // keep track of how many files failed to load for the current game so we can display a
          // message to the user informing them why some of their saves may not be loaded
          ++failed_loads_current_game;
        }
        continue;
      }

      // leave some free blocks for new saves that might be created
      const int free_blocks = m_bat1.m_free_blocks;
      const int gci_blocks = gci.m_gci_header.m_block_count;
      if (free_blocks - gci_blocks < reserved_blocks)
        continue;

      LoadGCI();
    }
  }
  path_list_free(&filenames);
  if (m_num_saves < Memcard::DIRLEN)
    ResetSave(m_num_saves);

  if (failed_loads_current_game > 0)
  {
    Core::DisplayMessage("Warning: Save file(s) of the current game failed to load.");
  }

  m_dir1.FixChecksums();
  m_dir2 = m_dir1;
  m_bat2 = m_bat1;

#if !defined(SKIP_GCMEMCARD_THREAD)
  m_flush_thread = sthread_create(Common::ThreadEntry<&GCMemcardDirectory::FlushThread>, this);
#endif
}

void GCMemcardDirectory::FlushThread()
{
  if (!Config::Get(Config::SESSION_SAVE_DATA_WRITABLE))
  {
    return;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "Memcard %s flushing thread",
           ExpansionInterface::SlotName(m_card_slot));
  Common::SetCurrentThreadName(thread_name);

  // About a second of frontend frames.
  constexpr u64 FLUSH_QUIET_FRAMES = 60;
  while (true)
  {
    // no-op until signalled
    m_flush_trigger.Wait();
    if (!m_flush_running.IsSet())
      return;

    // no-op as long as written to again within the quiet time
    for (;;)
    {
      const u64 quiet_from = static_cast<u64>(retro_atomic_load_acquire_64(&m_last_write_frame));
      if (!frame_clock_wait_until(quiet_from + FLUSH_QUIET_FRAMES, m_flush_running.Raw()))
        return;
      if (static_cast<u64>(retro_atomic_load_acquire_64(&m_last_write_frame)) == quiet_from)
        break;
    }

    WriteQueuedJobs();
  }
}

GCMemcardDirectory::~GCMemcardDirectory()
{
  m_flush_running.Clear();
  m_flush_trigger.Set();
  frame_clock_wake_all();

  if (m_flush_thread)
    Common::JoinThread(m_flush_thread);

  FlushToFile();
  std::free(m_blocks);
}

s32 GCMemcardDirectory::Read(u32 src_address, s32 length, u8* dest_address)
{
  const s32 block = src_address / Memcard::BLOCK_SIZE;
  const u32 offset = src_address % Memcard::BLOCK_SIZE;
  s32 extra = 0;  // used for read calls that are across multiple blocks

  if (offset + length > Memcard::BLOCK_SIZE)
  {
    extra = length + offset - Memcard::BLOCK_SIZE;
    length -= extra;

    // verify that we haven't calculated a length beyond BLOCK_SIZE
    DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, (src_address + length) % Memcard::BLOCK_SIZE == 0,
                     "Memcard directory Read Logic Error");
  }

  if (m_last_block != block)
  {
    switch (block)
    {
    case 0:
      m_last_block = block;
      m_last_block_address = (u8*)&m_hdr;
      break;
    case 1:
      m_last_block = -1;
      m_last_block_address = (u8*)&m_dir1;
      break;
    case 2:
      m_last_block = -1;
      m_last_block_address = (u8*)&m_dir2;
      break;
    case 3:
      m_last_block = block;
      m_last_block_address = (u8*)&m_bat1;
      break;
    case 4:
      m_last_block = block;
      m_last_block_address = (u8*)&m_bat2;
      break;
    default:
      m_last_block = SaveAreaRW(block);

      if (m_last_block == -1)
      {
        memset(dest_address, 0xFF, length);
        return 0;
      }
    }
  }

  memcpy(dest_address, m_last_block_address + offset, length);
  if (extra)
    extra = Read(src_address + length, extra, dest_address + length);
  return length + extra;
}

s32 GCMemcardDirectory::Write(u32 dest_address, s32 length, const u8* src_address)
{
  if (length != 0x80)
    INFO_LOG(EXPANSIONINTERFACE, "Writing to %#x. Length: %#x", dest_address, (unsigned)length);
  const s32 block = dest_address / Memcard::BLOCK_SIZE;
  const u32 offset = dest_address % Memcard::BLOCK_SIZE;
  s32 extra = 0;  // used for write calls that are across multiple blocks

  if (offset + length > Memcard::BLOCK_SIZE)
  {
    extra = length + offset - Memcard::BLOCK_SIZE;
    length -= extra;

    // verify that we haven't calculated a length beyond BLOCK_SIZE
    DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, (dest_address + length) % Memcard::BLOCK_SIZE == 0,
                     "Memcard directory Write Logic Error");
  }
  if (m_last_block != block)
  {
    switch (block)
    {
    case 0:
      m_last_block = block;
      m_last_block_address = (u8*)&m_hdr;
      break;
    case 1:
    case 2:
    {
      m_last_block = -1;
      s32 bytes_written = 0;
      while (length > 0)
      {
        const s32 to_write = length < (s32)Memcard::DENTRY_SIZE ? length : (s32)Memcard::DENTRY_SIZE;
        bytes_written +=
            DirectoryWrite(dest_address + bytes_written, to_write, src_address + bytes_written);
        length -= to_write;
      }
      return bytes_written;
    }
    case 3:
      m_last_block = block;
      m_last_block_address = (u8*)&m_bat1;
      break;
    case 4:
      m_last_block = block;
      m_last_block_address = (u8*)&m_bat2;
      break;
    default:
      m_last_block = SaveAreaRW(block, true);
      if (m_last_block == -1)
      {
        PanicAlert("Report: GCIFolder Writing to unallocated block %#x", (unsigned)block);
        exit(0);
      }
    }
  }

  memcpy(m_last_block_address + offset, src_address, length);

  if (extra)
    extra = Write(dest_address + length, extra, src_address + length);
  if (offset + length == Memcard::BLOCK_SIZE)
  {
    QueueDirtySaves();
    retro_atomic_store_release_64(&m_last_write_frame, static_cast<int64_t>(frame_clock_now()));
    m_flush_trigger.Set();
  }
  return length + extra;
}

void GCMemcardDirectory::ClearBlock(u32 address)
{
  if (address % Memcard::BLOCK_SIZE)
  {
    PanicAlert("GCMemcardDirectory: ClearBlock called with invalid block address");
    return;
  }

  const u32 block = address / Memcard::BLOCK_SIZE;
  INFO_LOG(EXPANSIONINTERFACE, "Clearing block %u", block);
  switch (block)
  {
  case 0:
    m_last_block = block;
    m_last_block_address = (u8*)&m_hdr;
    break;
  case 1:
    m_last_block = -1;
    m_last_block_address = (u8*)&m_dir1;
    break;
  case 2:
    m_last_block = -1;
    m_last_block_address = (u8*)&m_dir2;
    break;
  case 3:
    m_last_block = block;
    m_last_block_address = (u8*)&m_bat1;
    break;
  case 4:
    m_last_block = block;
    m_last_block_address = (u8*)&m_bat2;
    break;
  default:
    m_last_block = SaveAreaRW(block, true);
    if (m_last_block == -1)
      return;
  }
  std::memset(m_last_block_address, 0xFF, Memcard::BLOCK_SIZE);
}

inline void GCMemcardDirectory::SyncSaves()
{
  Memcard::Directory* current = &m_dir2;

  if (m_dir1.m_update_counter > m_dir2.m_update_counter)
  {
    current = &m_dir1;
  }

  for (u32 i = 0; i < Memcard::DIRLEN; ++i)
  {
    if (std::memcmp(current->m_dir_entries[i].m_gamecode, Memcard::DEntry::UNINITIALIZED_GAMECODE,
                    sizeof(Memcard::DEntry::UNINITIALIZED_GAMECODE)) != 0)
    {
      INFO_LOG(EXPANSIONINTERFACE, "Syncing save %#x",
               Common::swap32(current->m_dir_entries[i].m_gamecode));
      bool added = false;
      while (i >= m_num_saves)
      {
        ResetSave(m_num_saves++);
        added = true;
      }

      if (added || memcmp((u8*)&(m_saves[i].m_gci_header), (u8*)&(current->m_dir_entries[i]),
                          Memcard::DENTRY_SIZE))
      {
        m_saves[i].m_dirty = true;
        const u32 gamecode = Common::swap32(m_saves[i].m_gci_header.m_gamecode);
        const u32 new_gamecode = Common::swap32(current->m_dir_entries[i].m_gamecode);
        const u32 old_start = m_saves[i].m_gci_header.m_first_block;
        const u32 new_start = current->m_dir_entries[i].m_first_block;

        if ((gamecode != 0xFFFFFFFF) && (gamecode != new_gamecode))
        {
          PanicAlert("Game overwrote with another games save. Data corruption ahead %#x, %#x",
                     Common::swap32(m_saves[i].m_gci_header.m_gamecode),
                     Common::swap32(current->m_dir_entries[i].m_gamecode));
        }
        memcpy((u8*)&(m_saves[i].m_gci_header), (u8*)&(current->m_dir_entries[i]),
               Memcard::DENTRY_SIZE);
        if (old_start != new_start)
        {
          INFO_LOG(EXPANSIONINTERFACE, "Save moved from %#x to %#x", old_start, new_start);
          m_saves[i].m_num_used_blocks = 0;
          m_saves[i].m_num_save_blocks = 0;
        }
        if (!m_saves[i].m_num_used_blocks)
        {
          SetUsedBlocks(i);
        }
      }
    }
    else if ((i < m_num_saves) && (*(u32*)&(m_saves[i].m_gci_header) != 0xFFFFFFFF))
    {
      INFO_LOG(EXPANSIONINTERFACE, "Clearing and/or deleting save %#x",
               Common::swap32(m_saves[i].m_gci_header.m_gamecode));
      std::memcpy(m_saves[i].m_gci_header.m_gamecode, Memcard::DEntry::UNINITIALIZED_GAMECODE,
                  sizeof(m_saves[i].m_gci_header.m_gamecode));
      m_saves[i].m_num_save_blocks = 0;
      m_saves[i].m_num_used_blocks = 0;
      m_saves[i].m_dirty = true;
    }
  }
}
inline s32 GCMemcardDirectory::SaveAreaRW(u32 block, bool writing)
{
  if (!m_blocks)
    return -1;
  for (u16 i = 0; i < m_num_saves; ++i)
  {
    Memcard::GCIFile& save = m_saves[i];
    if (std::memcmp(save.m_gci_header.m_gamecode, Memcard::DEntry::UNINITIALIZED_GAMECODE,
                    sizeof(Memcard::DEntry::UNINITIALIZED_GAMECODE)) != 0)
    {
      if (!save.m_num_used_blocks)
      {
        SetUsedBlocks(i);
      }

      const int idx = save.UsesBlock(block);
      if (idx >= 0)
      {
        if (!save.LoadSaveBlocks(m_blocks))
          save.EraseSaveBlocks(m_blocks);

        if (writing)
        {
          save.m_dirty = true;
        }

        Memcard::GCMBlock* const data = save.SaveBlock(m_blocks, idx);
        if (idx >= save.m_num_save_blocks || !data)
        {
          PanicAlert("Block index (%d) is larger than the number of available blocks (%u)", idx,
                     (unsigned)save.m_num_save_blocks);
          return -1;
        }
        m_last_block = block;
        m_last_block_address = data->m_block;
        return m_last_block;
      }
    }
  }
  return -1;
}

s32 GCMemcardDirectory::DirectoryWrite(u32 dest_address, u32 length, const u8* src_address)
{
  const u32 block = dest_address / Memcard::BLOCK_SIZE;
  const u32 offset = dest_address % Memcard::BLOCK_SIZE;
  Memcard::Directory* dest = (block == 1) ? &m_dir1 : &m_dir2;
  const u16 Dnum = offset / Memcard::DENTRY_SIZE;

  if (Dnum == Memcard::DIRLEN)
  {
    // first 58 bytes should always be 0xff
    // needed to update the update ctr, checksums
    // could check for writes to the 6 important bytes but doubtful that it improves performance
    // noticeably
    memcpy((u8*)(dest) + offset, src_address, length);
    SyncSaves();
  }
  else
    memcpy((u8*)(dest) + offset, src_address, length);
  return length;
}

bool GCMemcardDirectory::SetUsedBlocks(int save_index)
{
  Memcard::BlockAlloc* current_bat;
  if (m_bat2.m_update_counter > m_bat1.m_update_counter)
    current_bat = &m_bat2;
  else
    current_bat = &m_bat1;

  Memcard::GCIFile& save = m_saves[save_index];
  u16 block = save.m_gci_header.m_first_block;
  save.m_num_used_blocks = 0;
  while (block != 0xFFFF)
  {
    if (save.m_num_used_blocks == Memcard::GCI_MAX_BLOCKS)
    {
      PanicAlert("BAT incorrect. Dolphin will now exit");
      exit(0);
    }
    save.m_used_blocks[save.m_num_used_blocks++] = block;
    block = current_bat->GetNextBlock(block);
    if (block == 0)
    {
      PanicAlert("BAT incorrect. Dolphin will now exit");
      exit(0);
    }
  }

  const u16 num_blocks = save.m_gci_header.m_block_count;
  const u16 blocks_from_bat = save.m_num_used_blocks;
  if (blocks_from_bat != num_blocks)
  {
    PanicAlert("Warning: Number of blocks indicated by the BAT (%u) does not match that of the "
               "loaded file header (%u)",
               (unsigned)blocks_from_bat, (unsigned)num_blocks);
    return false;
  }

  return true;
}

void GCMemcardDirectory::FlushToFile()
{
  QueueDirtySaves();
  WriteQueuedJobs();
}

void GCMemcardDirectory::PostFlushJob(const Memcard::GCIFile& save, bool retire)
{
  const u32 num_blocks = retire ? 0 : save.m_num_save_blocks;
  const size_t blocks_size = size_t(num_blocks) * Memcard::BLOCK_SIZE;
  const size_t name_size = strlen(save.m_filename) + 1;
  u32 i;
  auto* job =
      static_cast<FlushJob*>(std::malloc(offsetof(FlushJob, tail) + blocks_size + name_size));
  if (!job)
    return;
  static_assert(sizeof(Memcard::GCMBlock) == Memcard::BLOCK_SIZE);
  static_assert(std::is_trivially_copyable_v<Memcard::DEntry>);
  job->num_blocks = num_blocks;
  job->retire = retire;
  if (!retire)
  {
    job->header = save.m_gci_header;
    for (i = 0; i < num_blocks; i++)
    {
      const Memcard::GCMBlock* const block = save.SaveBlock(m_blocks, i);
      if (block)
        std::memcpy(job->tail + size_t(i) * Memcard::BLOCK_SIZE, block, Memcard::BLOCK_SIZE);
      else
        std::memset(job->tail + size_t(i) * Memcard::BLOCK_SIZE, 0xFF, Memcard::BLOCK_SIZE);
    }
  }
  std::memcpy(job->tail + blocks_size, save.m_filename, name_size);
  job->filename = reinterpret_cast<const char*>(job->tail + blocks_size);
  mpsc_stack_push(&m_flush_jobs, &job->link);
}

void GCMemcardDirectory::QueueDirtySaves()
{
  for (u32 i = 0; i < m_num_saves; i++)
  {
    Memcard::GCIFile& save = m_saves[i];
    if (save.m_dirty)
    {
      if (std::memcmp(save.m_gci_header.m_gamecode, Memcard::DEntry::UNINITIALIZED_GAMECODE,
                      sizeof(Memcard::DEntry::UNINITIALIZED_GAMECODE)) != 0)
      {
        save.m_dirty = false;
        if (!save.m_num_save_blocks)
        {
          // The save's header has been changed but the actual save blocks haven't been read/written
          // to
          // skip flushing this file until actual save data is modified
          ERROR_LOG(EXPANSIONINTERFACE,
                    "GCI header modified without corresponding save data changes");
          continue;
        }
        if (!save.HasFilename())
        {
          // The directory, then the generated name, both cut short to fit if they must.
          char* const default_save_name = save.m_filename;
          const size_t name_size = sizeof(save.m_filename);
          strlcpy(default_save_name, m_save_directory, name_size);
          const size_t directory_length = std::strlen(default_save_name);
          GenerateDefaultGCIFilename(save.m_gci_header, m_hdr.IsShiftJIS(),
                                     default_save_name + directory_length,
                                     name_size - directory_length);

          // Check to see if another file is using the same name
          // This seems unlikely except in the case of file corruption
          // otherwise what user would name another file this way?
          for (int j = 0; File::Exists(default_save_name) && j < 10; ++j)
          {
            // Insert a '0' before the ".gci", as long as there is room for it.
            const size_t length = std::strlen(default_save_name);
            if (length < 4 || length + 1 >= name_size)
              break;
            std::memmove(default_save_name + length - 3, default_save_name + length - 4, 5);
            default_save_name[length - 4] = '0';
          }
          if (File::Exists(default_save_name))
          {
            PanicAlert("Failed to find new filename.\n%s\n will be overwritten", default_save_name);
          }
        }
        PostFlushJob(save, false);
      }
      else if (save.HasFilename())
      {
        save.m_dirty = false;
        PostFlushJob(save, true);
        save.m_filename[0] = '\0';
        save.m_num_save_blocks = 0;
        save.m_num_used_blocks = 0;
      }
    }

    // Unload the save data for any game that is not running
    // we could use !m_dirty, but some games have multiple gci files and may not write to them
    // simultaneously
    // this ensures that the save data for all of the current games gci files are stored in the
    // savestate
    const u32 gamecode = Common::swap32(save.m_gci_header.m_gamecode);
    if (gamecode != m_game_id && gamecode != 0xFFFFFFFF && save.m_num_save_blocks)
    {
      INFO_LOG(EXPANSIONINTERFACE, "Flushing savedata to disk for %s", save.m_filename);
      save.m_num_save_blocks = 0;
    }
  }
}

void GCMemcardDirectory::WriteQueuedJobs()
{
  // Oldest first, so a later job for the same file wins.
  mpsc_stack_node_t* const jobs = mpsc_stack_reverse(mpsc_stack_drain(&m_flush_jobs));
  mpsc_stack_node_t* node;

  for (node = jobs; node; node = node->next)
  {
    const FlushJob* const job = reinterpret_cast<const FlushJob*>(node);
    const mpsc_stack_node_t* later;
    for (later = node->next; later; later = later->next)
    {
      if (!std::strcmp(reinterpret_cast<const FlushJob*>(later)->filename, job->filename))
        break;
    }
    if (later)
      continue;

    if (job->retire)
    {
      char deleted_name[sizeof(Memcard::GCIFile::m_filename) + 8];
      snprintf(deleted_name, sizeof(deleted_name), "%s.deleted", job->filename);
      if (File::Exists(deleted_name))
        File::Delete(deleted_name);
      File::Rename(job->filename, deleted_name);
      continue;
    }

    // replaced whole or left as it was: a save is never half written
    RFILE* gci = filestream_open_atomic(job->filename);
    if (!gci)
    {
      ERROR_LOG(EXPANSIONINTERFACE, "Failed to open file at %s for writing", job->filename);
      continue;
    }
    const s64 blocks_size = s64(job->num_blocks) * Memcard::BLOCK_SIZE;
    const bool ok =
        filestream_write(gci, &job->header, Memcard::DENTRY_SIZE) == Memcard::DENTRY_SIZE &&
        (!blocks_size || filestream_write(gci, job->tail, blocks_size) == blocks_size);
    if (filestream_commit_atomic(gci, job->filename, ok) != 0)
      ERROR_LOG(EXPANSIONINTERFACE, "Failed to save data to %s", job->filename);
  }

  for (node = jobs; node;)
  {
    mpsc_stack_node_t* const next = node->next;
    std::free(node);
    node = next;
  }
}

void GCMemcardDirectory::DoState(PointerWrap& p)
{
  m_last_block = -1;
  m_last_block_address = nullptr;
  DoString(p, m_save_directory, sizeof(m_save_directory));
  p.Do(m_hdr);
  p.Do(m_dir1);
  p.Do(m_dir2);
  p.Do(m_bat1);
  p.Do(m_bat2);
  // as the std::vector of them was: a u32 count, then each
  u32 count = m_num_saves;
  p.Do(count);
  if (count > Memcard::DIRLEN || !m_blocks)
  {
    p.SetMeasureMode();
    return;
  }
  for (u32 i = 0; i < count; i++)
    m_saves[i].DoState(p, m_blocks);
  if (p.IsReadMode())
    m_num_saves = count;
}

void MigrateFromMemcardFile(const std::string& directory_name, ExpansionInterface::Slot card_slot,
                            DiscIO::Region region)
{
  File::CreateFullPath(directory_name);
  const std::string ini_memcard = Config::GetMemcardPath(card_slot, region);
  if (File::Exists(ini_memcard))
  {
    // The card's system blocks alone are 40 KiB, too large for the stack
    Memcard::GCMemcard* const memcard = new Memcard::GCMemcard;
    const Memcard::GCMemcardErrorCode error_code = memcard->Open(ini_memcard);
    if (!error_code.HasCriticalErrors() && memcard->IsValid())
    {
      for (u8 i = 0; i < Memcard::DIRLEN; i++)
      {
        Memcard::Savefile savefile;
        if (!memcard->ExportFile(i, &savefile))
          continue;

        std::string filepath =
            directory_name + DIR_SEP + Memcard::GenerateFilename(savefile.dir_entry) + ".gci";
        Memcard::WriteGCISavefile(filepath, savefile);
      }
    }
    delete memcard;
  }
}
