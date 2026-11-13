// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/GCMemcardDirectory.h"
#include "Core/HW/EXI/EXI.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

static std::string GenerateDefaultGCIFilename(const Memcard::DEntry& entry,
                                              bool card_encoding_is_shift_jis)
{
  const auto string_decoder = card_encoding_is_shift_jis ? SHIFTJISToUTF8 : CP1252ToUTF8;
  const auto strip_null = [](const std::string_view& s) {
    const auto offset = s.find('\0');
    if (offset == std::string_view::npos)
      return s;
    return s.substr(0, offset);
  };

  const std::string_view makercode(reinterpret_cast<const char*>(entry.m_makercode.data()),
                                   entry.m_makercode.size());
  const std::string_view gamecode(reinterpret_cast<const char*>(entry.m_gamecode.data()),
                                  entry.m_gamecode.size());
  const std::string_view filename(reinterpret_cast<const char*>(entry.m_filename.data()),
                                  entry.m_filename.size());
  std::string name(strip_null(string_decoder(makercode)));
  name += '-';
  name += strip_null(string_decoder(gamecode));
  name += '-';
  name += strip_null(string_decoder(filename));
  name += ".gci";
  return Common::EscapeFileName(name);
}

bool GCMemcardDirectory::LoadGCI(Memcard::GCIFile gci)
{
  // check if any already loaded file has the same internal name as the new file
  for (const Memcard::GCIFile& already_loaded_gci : m_saves)
  {
    if (HasSameIdentity(gci.m_gci_header, already_loaded_gci.m_gci_header))
    {
      ERROR_LOG(EXPANSIONINTERFACE,
                "%s\nwas not loaded because it has the same internal filename as previously "
                "loaded save\n%s",
                gci.m_filename.c_str(), already_loaded_gci.m_filename.c_str());
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
              gci.m_filename.c_str(), (unsigned)num_blocks);
    return false;
  }

  if (!gci.LoadSaveBlocks())
  {
    ERROR_LOG(EXPANSIONINTERFACE, "Failed to load data of %s", gci.m_filename.c_str());
    return false;
  }

  // reserve storage for the save file in the BAT
  const u16 first_block = m_bat1.AssignBlocksContiguous(num_blocks);
  if (first_block == 0xFFFF)
  {
    ERROR_LOG(
        EXPANSIONINTERFACE,
        "%s\nwas not loaded because there are not enough free blocks on the virtual memory card",
        gci.m_filename.c_str());
    return false;
  }
  gci.m_gci_header.m_first_block = first_block;

  if (gci.HasCopyProtection())
  {
    Memcard::GCMemcard::PSO_MakeSaveGameValid(m_hdr, gci.m_gci_header, gci.m_save_data);
    Memcard::GCMemcard::FZEROGX_MakeSaveGameValid(m_hdr, gci.m_gci_header, gci.m_save_data);
  }

  // actually load save file into memory card
  const int idx = (int)m_saves.size();
  m_dir1.Replace(gci.m_gci_header, idx);
  m_saves.push_back(std::move(gci));
  SetUsedBlocks(idx);

  return true;
}

GCMemcardDirectory::GCMemcardDirectory(std::string directory, ExpansionInterface::Slot slot,
                                       const Memcard::HeaderData& header_data, u32 game_id)
    : MemoryCardBase(slot, header_data.m_size_mb), m_game_id(game_id), m_last_block(-1),
      m_hdr(header_data), m_bat1(header_data.m_size_mb), m_saves(0),
      m_save_directory(std::move(directory))
{
  // Use existing header data if available
  {
    File::IOFile((m_save_directory + MC_HDR), "rb").ReadBytes(&m_hdr, Memcard::BLOCK_SIZE);
  }

  const bool current_game_only = Config::Get(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY);
  const std::vector<std::string> filenames = Common::DoFileSearch(m_save_directory, ".gci");

  // split up into files for current games we should definitely load,
  // and files for other games that we don't care too much about
  std::vector<Memcard::GCIFile> gci_current_game;
  std::vector<Memcard::GCIFile> gci_other_games;
  for (const std::string& filename : filenames)
  {
    Memcard::GCIFile gci;
    gci.m_filename = filename;
    gci.m_dirty = false;
    if (!gci.LoadHeader())
    {
      ERROR_LOG(EXPANSIONINTERFACE, "Failed to load header of %s", filename.c_str());
      continue;
    }

    if (m_game_id == Common::swap32(gci.m_gci_header.m_gamecode.data()))
      gci_current_game.emplace_back(std::move(gci));
    else if (!current_game_only)
      gci_other_games.emplace_back(std::move(gci));
  }

  m_saves.reserve(Memcard::DIRLEN);

  // load files for current game
  size_t failed_loads_current_game = 0;
  for (Memcard::GCIFile& gci : gci_current_game)
  {
    if (!LoadGCI(std::move(gci)))
    {
      // keep track of how many files failed to load for the current game so we can display a
      // message to the user informing them why some of their saves may not be loaded
      ++failed_loads_current_game;
    }
  }

  // leave about 10% of free space on the card if possible
  const int total_blocks = Memcard::MbitToFreeBlocks(m_hdr.m_data.m_size_mb);
  const int reserved_blocks = total_blocks / 10;

  // load files for other games
  for (Memcard::GCIFile& gci : gci_other_games)
  {
    // leave some free file entries for new saves that might be created
    if (m_saves.size() > 112)
      break;

    // leave some free blocks for new saves that might be created
    const int free_blocks = m_bat1.m_free_blocks;
    const int gci_blocks = gci.m_gci_header.m_block_count;
    if (free_blocks - gci_blocks < reserved_blocks)
      continue;

    LoadGCI(std::move(gci));
  }

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
    if (current->m_dir_entries[i].m_gamecode != Memcard::DEntry::UNINITIALIZED_GAMECODE)
    {
      INFO_LOG(EXPANSIONINTERFACE, "Syncing save %#x",
               Common::swap32(current->m_dir_entries[i].m_gamecode.data()));
      bool added = false;
      while (i >= m_saves.size())
      {
        Memcard::GCIFile temp;
        m_saves.push_back(temp);
        added = true;
      }

      if (added || memcmp((u8*)&(m_saves[i].m_gci_header), (u8*)&(current->m_dir_entries[i]),
                          Memcard::DENTRY_SIZE))
      {
        m_saves[i].m_dirty = true;
        const u32 gamecode = Common::swap32(m_saves[i].m_gci_header.m_gamecode.data());
        const u32 new_gamecode = Common::swap32(current->m_dir_entries[i].m_gamecode.data());
        const u32 old_start = m_saves[i].m_gci_header.m_first_block;
        const u32 new_start = current->m_dir_entries[i].m_first_block;

        if ((gamecode != 0xFFFFFFFF) && (gamecode != new_gamecode))
        {
          PanicAlert("Game overwrote with another games save. Data corruption ahead %#x, %#x",
                     Common::swap32(m_saves[i].m_gci_header.m_gamecode.data()),
                     Common::swap32(current->m_dir_entries[i].m_gamecode.data()));
        }
        memcpy((u8*)&(m_saves[i].m_gci_header), (u8*)&(current->m_dir_entries[i]),
               Memcard::DENTRY_SIZE);
        if (old_start != new_start)
        {
          INFO_LOG(EXPANSIONINTERFACE, "Save moved from %#x to %#x", old_start, new_start);
          m_saves[i].m_used_blocks.clear();
          m_saves[i].m_save_data.clear();
        }
        if (m_saves[i].m_used_blocks.empty())
        {
          SetUsedBlocks(i);
        }
      }
    }
    else if ((i < m_saves.size()) && (*(u32*)&(m_saves[i].m_gci_header) != 0xFFFFFFFF))
    {
      INFO_LOG(EXPANSIONINTERFACE, "Clearing and/or deleting save %#x",
               Common::swap32(m_saves[i].m_gci_header.m_gamecode.data()));
      m_saves[i].m_gci_header.m_gamecode = Memcard::DEntry::UNINITIALIZED_GAMECODE;
      m_saves[i].m_save_data.clear();
      m_saves[i].m_used_blocks.clear();
      m_saves[i].m_dirty = true;
    }
  }
}
inline s32 GCMemcardDirectory::SaveAreaRW(u32 block, bool writing)
{
  for (u16 i = 0; i < m_saves.size(); ++i)
  {
    if (m_saves[i].m_gci_header.m_gamecode != Memcard::DEntry::UNINITIALIZED_GAMECODE)
    {
      if (m_saves[i].m_used_blocks.empty())
      {
        SetUsedBlocks(i);
      }

      const int idx = m_saves[i].UsesBlock(block);
      if (idx >= 0)
      {
        if (!m_saves[i].LoadSaveBlocks())
        {
          int num_blocks = m_saves[i].m_gci_header.m_block_count;
          while (num_blocks)
          {
            m_saves[i].m_save_data.emplace_back();
            num_blocks--;
          }
        }

        if (writing)
        {
          m_saves[i].m_dirty = true;
        }

        if (static_cast<size_t>(idx) >= m_saves[i].m_save_data.size())
        {
          PanicAlert("Block index (%d) is larger than the number of available blocks (%" PRIu64 ")",
                     idx, (u64)m_saves[i].m_save_data.size());
          return -1;
        }
        m_last_block = block;
        m_last_block_address = m_saves[i].m_save_data[idx].m_block.data();
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

  u16 block = m_saves[save_index].m_gci_header.m_first_block;
  while (block != 0xFFFF)
  {
    m_saves[save_index].m_used_blocks.push_back(block);
    block = current_bat->GetNextBlock(block);
    if (block == 0)
    {
      PanicAlert("BAT incorrect. Dolphin will now exit");
      exit(0);
    }
  }

  const u16 num_blocks = m_saves[save_index].m_gci_header.m_block_count;
  const u16 blocks_from_bat = static_cast<u16>(m_saves[save_index].m_used_blocks.size());
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
  const u32 num_blocks = retire ? 0 : static_cast<u32>(save.m_save_data.size());
  const size_t blocks_size = size_t(num_blocks) * Memcard::BLOCK_SIZE;
  const size_t name_size = save.m_filename.size() + 1;
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
    std::memcpy(job->tail, save.m_save_data.data(), blocks_size);
  }
  std::memcpy(job->tail + blocks_size, save.m_filename.c_str(), name_size);
  job->filename = reinterpret_cast<const char*>(job->tail + blocks_size);
  mpsc_stack_push(&m_flush_jobs, &job->link);
}

void GCMemcardDirectory::QueueDirtySaves()
{

  for (Memcard::GCIFile& save : m_saves)
  {
    if (save.m_dirty)
    {
      if (save.m_gci_header.m_gamecode != Memcard::DEntry::UNINITIALIZED_GAMECODE)
      {
        save.m_dirty = false;
        if (save.m_save_data.empty())
        {
          // The save's header has been changed but the actual save blocks haven't been read/written
          // to
          // skip flushing this file until actual save data is modified
          ERROR_LOG(EXPANSIONINTERFACE,
                    "GCI header modified without corresponding save data changes");
          continue;
        }
        if (save.m_filename.empty())
        {
          std::string default_save_name =
              m_save_directory + GenerateDefaultGCIFilename(save.m_gci_header, m_hdr.IsShiftJIS());

          // Check to see if another file is using the same name
          // This seems unlikely except in the case of file corruption
          // otherwise what user would name another file this way?
          for (int j = 0; File::Exists(default_save_name) && j < 10; ++j)
          {
            default_save_name.insert(default_save_name.end() - 4, '0');
          }
          if (File::Exists(default_save_name))
          {
            PanicAlert("Failed to find new filename.\n%s\n will be overwritten",
                       default_save_name.c_str());
          }
          save.m_filename = default_save_name;
        }
        PostFlushJob(save, false);
      }
      else if (save.m_filename.length() != 0)
      {
        save.m_dirty = false;
        PostFlushJob(save, true);
        save.m_filename.clear();
        save.m_save_data.clear();
        save.m_used_blocks.clear();
      }
    }

    // Unload the save data for any game that is not running
    // we could use !m_dirty, but some games have multiple gci files and may not write to them
    // simultaneously
    // this ensures that the save data for all of the current games gci files are stored in the
    // savestate
    const u32 gamecode = Common::swap32(save.m_gci_header.m_gamecode.data());
    if (gamecode != m_game_id && gamecode != 0xFFFFFFFF && !save.m_save_data.empty())
    {
      INFO_LOG(EXPANSIONINTERFACE, "Flushing savedata to disk for %s", save.m_filename.c_str());
      save.m_save_data.clear();
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
      const std::string name = job->filename;
      const std::string deleted_name = name + ".deleted";
      if (File::Exists(deleted_name))
        File::Delete(deleted_name);
      File::Rename(name, deleted_name);
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
  p.Do(m_save_directory);
  p.Do(m_hdr);
  p.Do(m_dir1);
  p.Do(m_dir2);
  p.Do(m_bat1);
  p.Do(m_bat2);
  p.DoEachElement(m_saves, [](PointerWrap& p_, Memcard::GCIFile& save) { save.DoState(p_); });
}

void MigrateFromMemcardFile(const std::string& directory_name, ExpansionInterface::Slot card_slot,
                            DiscIO::Region region)
{
  File::CreateFullPath(directory_name);
  const std::string ini_memcard = Config::GetMemcardPath(card_slot, region);
  if (File::Exists(ini_memcard))
  {
    auto [error_code, memcard] = Memcard::GCMemcard::Open(ini_memcard.c_str());
    if (!error_code.HasCriticalErrors() && memcard && memcard->IsValid())
    {
      for (u8 i = 0; i < Memcard::DIRLEN; i++)
      {
        const auto savefile = memcard->ExportFile(i);
        if (!savefile)
          continue;

        std::string filepath =
            directory_name + DIR_SEP + Memcard::GenerateFilename(savefile->dir_entry) + ".gci";
        Memcard::WriteGCISavefile(filepath, *savefile);
      }
    }
  }
}
