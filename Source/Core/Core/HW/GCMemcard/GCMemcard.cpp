// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/GCMemcard.h"

#include <cstring>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/HW/GCMemcard/GCMemcardUtils.h"

// 0 for a size that is not a whole number of megabits
static constexpr u64 BytesToMegabits(u64 bytes)
{
  const u64 factor = ((1024 * 1024) / 8);
  const u64 megabits = bytes / factor;
  const u64 remainder = bytes % factor;
  if (remainder != 0)
    return 0;
  return megabits;
}

namespace Memcard
{
// unused areas hold 0xFF throughout
static bool IsFilledWithFF(const u8* data, size_t size)
{
  size_t i;
  for (i = 0; i < size; i++)
  {
    if (data[i] != 0xFF)
      return false;
  }
  return true;
}

bool GCMemcardErrorCode::HasCriticalErrors() const
{
  return Test(GCMemcardValidityIssues::FAILED_TO_OPEN) || Test(GCMemcardValidityIssues::IO_ERROR) ||
         Test(GCMemcardValidityIssues::INVALID_CARD_SIZE) ||
         Test(GCMemcardValidityIssues::INVALID_CHECKSUM) ||
         Test(GCMemcardValidityIssues::MISMATCHED_CARD_SIZE) ||
         Test(GCMemcardValidityIssues::FREE_BLOCK_MISMATCH) ||
         Test(GCMemcardValidityIssues::DIR_BAT_INCONSISTENT);
}

bool GCMemcardErrorCode::Test(GCMemcardValidityIssues code) const
{
  return m_errors.test(static_cast<size_t>(code));
}

void GCMemcardErrorCode::Set(GCMemcardValidityIssues code)
{
  m_errors.set(static_cast<size_t>(code));
}

GCMemcardErrorCode& GCMemcardErrorCode::operator|=(const GCMemcardErrorCode& other)
{
  this->m_errors |= other.m_errors;
  return *this;
}

GCMemcard::GCMemcard()
    : m_valid(false), m_size_blocks(0), m_size_mb(0), m_active_directory(0), m_active_bat(0)
{
}

GCMemcardErrorCode GCMemcard::Open(const std::string& filename)
{
  GCMemcardErrorCode error_code;
  File::IOFile file(filename, "rb");
  if (!file.IsOpen())
  {
    error_code.Set(GCMemcardValidityIssues::FAILED_TO_OPEN);
    return error_code;
  }

  // check if the filesize is a valid memory card size
  const u64 filesize = file.GetSize();
  const u64 filesize_megabits = BytesToMegabits(filesize);
  const std::array<u16, 6> valid_megabits = {{
      MBIT_SIZE_MEMORY_CARD_59,
      MBIT_SIZE_MEMORY_CARD_123,
      MBIT_SIZE_MEMORY_CARD_251,
      MBIT_SIZE_MEMORY_CARD_507,
      MBIT_SIZE_MEMORY_CARD_1019,
      MBIT_SIZE_MEMORY_CARD_2043,
  }};

  size_t size_index;
  for (size_index = 0; size_index < valid_megabits.size(); size_index++)
  {
    if (valid_megabits[size_index] == filesize_megabits)
      break;
  }
  if (size_index == valid_megabits.size())
  {
    error_code.Set(GCMemcardValidityIssues::INVALID_CARD_SIZE);
    return error_code;
  }

  const u16 card_size_mbits = static_cast<u16>(filesize_megabits);

  // read the entire card into memory
  file.Seek(0, File::SeekOrigin::Begin);
  if (!file.ReadBytes(&m_header_block, BLOCK_SIZE) ||
      !file.ReadBytes(&m_directory_blocks[0], BLOCK_SIZE) ||
      !file.ReadBytes(&m_directory_blocks[1], BLOCK_SIZE) ||
      !file.ReadBytes(&m_bat_blocks[0], BLOCK_SIZE) ||
      !file.ReadBytes(&m_bat_blocks[1], BLOCK_SIZE))
  {
    error_code.Set(GCMemcardValidityIssues::IO_ERROR);
    return error_code;
  }

  const u16 card_size_blocks = card_size_mbits * MBIT_TO_BLOCKS;
  const u16 user_data_blocks = card_size_blocks - MC_FST_BLOCKS;
  m_data_blocks.reserve(user_data_blocks);
  for (u16 i = 0; i < user_data_blocks; ++i)
  {
    GCMBlock& block = m_data_blocks.emplace_back();
    if (!file.ReadArray(block.m_block.data(), BLOCK_SIZE))
    {
      error_code.Set(GCMemcardValidityIssues::IO_ERROR);
      return error_code;
    }
  }

  file.Close();

  m_size_blocks = card_size_blocks;
  m_size_mb = card_size_mbits;

  // can return invalid card size, invalid checksum, data in unused area
  // data in unused area is okay, otherwise fail
  const GCMemcardErrorCode header_error_code = m_header_block.CheckForErrors(card_size_mbits);
  error_code |= header_error_code;
  if (header_error_code.HasCriticalErrors())
    return error_code;

  // The GC BIOS counts any card as corrupted as long as at least any two of [dir0, dir1, bat0,
  // bat1] are corrupted. Yes, even if we have one valid dir and one valid bat, and even if those
  // are both supposedly the newer ones.
  //
  // If both blocks of a single category are non-corrupted the used block depends on the update
  // counter. If both blocks have the same update counter, it prefers block 0. Otherwise it prefers
  // whichever block has the higher value. Essentially, if (0.update_ctr >= 1.update_ctr) { use 0 }
  // else { use 1 }.
  //
  // If a single block of the four is corrupted, the non-corrupted one of the same category is
  // immediately copied over the corrupted block with an incremented update counter. At this point
  // both blocks contain the same data, so it's hard to tell which one is used, but presumably it
  // uses the one with the now-higher update counter, same as it would have otherwise.
  //
  // This rule only applies for errors within a single block! That is, invalid checksums for both
  // types, and free block mismatch for the BATs. Once two valid blocks have been selected but it
  // later turns out they do not match each other (eg. claimed block count of a file in the
  // directory does not match the actual block count arrived at by following BAT), the card will be
  // treated as corrupted, even if perhaps a different combination of the two blocks would result in
  // a valid memory

  // can return invalid checksum, data in unused area
  GCMemcardErrorCode dir_block_0_error_code = m_directory_blocks[0].CheckForErrors();
  GCMemcardErrorCode dir_block_1_error_code = m_directory_blocks[1].CheckForErrors();

  // can return invalid card size, invalid checksum, data in unused area, free block mismatch
  GCMemcardErrorCode bat_block_0_error_code = m_bat_blocks[0].CheckForErrors(card_size_mbits);
  GCMemcardErrorCode bat_block_1_error_code = m_bat_blocks[1].CheckForErrors(card_size_mbits);

  const bool dir_block_0_valid = !dir_block_0_error_code.HasCriticalErrors();
  const bool dir_block_1_valid = !dir_block_1_error_code.HasCriticalErrors();
  const bool bat_block_0_valid = !bat_block_0_error_code.HasCriticalErrors();
  const bool bat_block_1_valid = !bat_block_1_error_code.HasCriticalErrors();

  // if any two (at least) blocks are corrupted return failure
  // TODO: Consider allowing a recovery option when there's still a valid one of each type.
  int number_of_corrupted_dir_bat_blocks = 0;
  if (!dir_block_0_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!dir_block_1_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!bat_block_0_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!bat_block_1_valid)
    ++number_of_corrupted_dir_bat_blocks;

  if (number_of_corrupted_dir_bat_blocks > 1)
  {
    error_code |= dir_block_0_error_code;
    error_code |= dir_block_1_error_code;
    error_code |= bat_block_0_error_code;
    error_code |= bat_block_1_error_code;
    return error_code;
  }

  // if exactly one block is corrupted copy and update it over the non-corrupted block
  if (number_of_corrupted_dir_bat_blocks == 1)
  {
    if (!dir_block_0_valid)
    {
      m_directory_blocks[0] = m_directory_blocks[1];
      m_directory_blocks[0].m_update_counter = m_directory_blocks[0].m_update_counter + 1;
      m_directory_blocks[0].FixChecksums();
      dir_block_0_error_code = m_directory_blocks[0].CheckForErrors();
    }
    else if (!dir_block_1_valid)
    {
      m_directory_blocks[1] = m_directory_blocks[0];
      m_directory_blocks[1].m_update_counter = m_directory_blocks[1].m_update_counter + 1;
      m_directory_blocks[1].FixChecksums();
      dir_block_1_error_code = m_directory_blocks[1].CheckForErrors();
    }
    else if (!bat_block_0_valid)
    {
      m_bat_blocks[0] = m_bat_blocks[1];
      m_bat_blocks[0].m_update_counter = m_bat_blocks[0].m_update_counter + 1;
      m_bat_blocks[0].FixChecksums();
      bat_block_0_error_code = m_bat_blocks[0].CheckForErrors(card_size_mbits);
    }
    else if (!bat_block_1_valid)
    {
      m_bat_blocks[1] = m_bat_blocks[0];
      m_bat_blocks[1].m_update_counter = m_bat_blocks[1].m_update_counter + 1;
      m_bat_blocks[1].FixChecksums();
      bat_block_1_error_code = m_bat_blocks[1].CheckForErrors(card_size_mbits);
    }
    else
    {
      // should never reach here
      ASSERT(false);
    }
  }

  error_code |= dir_block_0_error_code;
  error_code |= dir_block_1_error_code;
  error_code |= bat_block_0_error_code;
  error_code |= bat_block_1_error_code;

  // select the in-use Dir and BAT blocks based on update counter

  // These are compared as signed values by the GC BIOS. There is no protection against overflow, so
  // if one block is MAX_VAL and the other is MIN_VAL it still picks the MAX_VAL one as the active
  // one, even if that results in a corrupted memory
  // TODO: We could try to be smarter about this to rescue seemingly-corrupted cards.

  if (m_directory_blocks[0].m_update_counter >= m_directory_blocks[1].m_update_counter)
    m_active_directory = 0;
  else
    m_active_directory = 1;

  if (m_bat_blocks[0].m_update_counter >= m_bat_blocks[1].m_update_counter)
    m_active_bat = 0;
  else
    m_active_bat = 1;

  // check for consistency between the active Dir and BAT
  const GCMemcardErrorCode dir_bat_consistency_error_code =
      GetActiveDirectory().CheckForErrorsWithBat(GetActiveBat());
  error_code |= dir_bat_consistency_error_code;
  if (error_code.HasCriticalErrors())
    return error_code;

  m_valid = true;

  return error_code;
}

const Directory& GCMemcard::GetActiveDirectory() const
{
  return m_directory_blocks[m_active_directory];
}

const BlockAlloc& GCMemcard::GetActiveBat() const
{
  return m_bat_blocks[m_active_bat];
}

static void CalculateMemcardChecksums(const u8* data, size_t size, u16* sum_out, u16* inv_out)
{
  ASSERT(size % 2 == 0);
  u16 csum = 0;
  u16 inv_csum = 0;

  for (size_t i = 0; i < size; i += 2)
  {
    const u16 d = Common::swap16(&data[i]);
    csum += d;
    inv_csum += static_cast<u16>(d ^ 0xffff);
  }

  csum = Common::swap16(csum);
  inv_csum = Common::swap16(inv_csum);

  if (csum == 0xffff)
    csum = 0;
  if (inv_csum == 0xffff)
    inv_csum = 0;

  *sum_out = csum;
  *inv_out = inv_csum;
}

u16 GCMemcard::DEntry_FirstBlock(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;

  const u16 block = GetActiveDirectory().m_dir_entries[index].m_first_block;
  if (block > (u16)m_size_blocks)
    return 0xFFFF;
  return block;
}

u16 GCMemcard::DEntry_BlockCount(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;

  const u16 blocks = GetActiveDirectory().m_dir_entries[index].m_block_count;
  if (blocks > (u16)m_size_blocks)
    return 0xFFFF;
  return blocks;
}

BlockAlloc::BlockAlloc(u16 size_mbits)
{
  memset(this, 0, BLOCK_SIZE);
  m_free_blocks = MbitToFreeBlocks(size_mbits);
  m_last_allocated_block = 4;
  FixChecksums();
}

u16 BlockAlloc::GetNextBlock(u16 block) const
{
  // FIXME: This is fishy, shouldn't that be in range [5, 4096[?
  if ((block < MC_FST_BLOCKS) || (block > 4091))
    return 0;

  return m_map[block - MC_FST_BLOCKS];
}

// Parameters and return value are expected as memory card block index,
// not BAT index; that is, block 5 is the first file data block.
void BlockAlloc::FixChecksums()
{
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  m_checksum = checksum_sum;
  m_checksum_inv = checksum_inv;
}

u16 BlockAlloc::AssignBlocksContiguous(u16 length)
{
  const u16 starting = m_last_allocated_block + 1;
  if (length > m_free_blocks)
    return 0xFFFF;
  u16 current = starting;
  while ((current - starting + 1) < length)
  {
    m_map[current - 5] = current + 1;
    current++;
  }
  m_map[current - 5] = 0xFFFF;
  m_last_allocated_block = current;
  m_free_blocks = m_free_blocks - length;
  FixChecksums();
  return starting;
}

void BlockAlloc::CalculateChecksums(u16* sum_out, u16* inv_out) const
{
  static_assert(std::is_trivially_copyable<BlockAlloc>());

  // the checksummed bytes read in place
  constexpr size_t checksum_area_start = offsetof(BlockAlloc, m_update_counter);
  constexpr size_t checksum_area_end = sizeof(BlockAlloc);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  CalculateMemcardChecksums(reinterpret_cast<const u8*>(this) + checksum_area_start,
                            checksum_area_size, sum_out, inv_out);
}

GCMemcardErrorCode BlockAlloc::CheckForErrors(u16 size_mbits) const
{
  GCMemcardErrorCode error_code;

  // verify checksums
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  if (size_mbits > 0 && size_mbits <= 256)
  {
    // check if free block count matches the actual amount of free blocks in m_map
    const u16 total_available_blocks = (size_mbits * MBIT_TO_BLOCKS) - MC_FST_BLOCKS;
    ASSERT(total_available_blocks <= m_map.size());
    u16 blocks_in_use = 0;
    for (size_t i = 0; i < total_available_blocks; ++i)
    {
      if (m_map[i] != 0)
        ++blocks_in_use;
    }
    const u16 free_blocks = total_available_blocks - blocks_in_use;

    if (free_blocks != m_free_blocks)
      error_code.Set(GCMemcardValidityIssues::FREE_BLOCK_MISMATCH);

    // remaining blocks map to nothing on hardware and must be empty
    for (size_t i = total_available_blocks; i < m_map.size(); ++i)
    {
      if (m_map[i] != 0)
      {
        error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
        break;
      }
    }
  }
  else
  {
    // card size is outside the range of blocks that can be addressed
    error_code.Set(GCMemcardValidityIssues::INVALID_CARD_SIZE);
  }

  return error_code;
}

GCMemcardGetSaveDataRetVal GCMemcard::GetSaveData(u8 index, std::vector<GCMBlock>& Blocks) const
{
  if (!m_valid)
    return GCMemcardGetSaveDataRetVal::NOMEMCARD;

  const u16 block = DEntry_FirstBlock(index);
  const u16 BlockCount = DEntry_BlockCount(index);

  if ((block == 0xFFFF) || (BlockCount == 0xFFFF))
  {
    return GCMemcardGetSaveDataRetVal::FAIL;
  }

  u16 nextBlock = block;
  for (int i = 0; i < BlockCount; ++i)
  {
    if ((!nextBlock) || (nextBlock == 0xFFFF))
      return GCMemcardGetSaveDataRetVal::FAIL;
    Blocks.push_back(m_data_blocks[nextBlock - MC_FST_BLOCKS]);
    nextBlock = GetActiveBat().GetNextBlock(nextBlock);
  }
  return GCMemcardGetSaveDataRetVal::SUCCESS;
}

bool GCMemcard::ExportFile(u8 index, Savefile* savefile) const
{
  if (!m_valid || index >= DIRLEN)
    return false;

  savefile->dir_entry = GetActiveDirectory().m_dir_entries[index];
  if (savefile->dir_entry.m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
    return false;

  return GetSaveData(index, savefile->blocks) == GCMemcardGetSaveDataRetVal::SUCCESS;
}

bool GCMemcard::Format(u8* card_data, const CardFlashId& flash_id, u16 size_mbits, bool shift_jis,
                       u32 rtc_bias, u32 sram_language, u64 format_time)
{
  if (!card_data)
    return false;

  const Header header(flash_id, size_mbits, shift_jis, rtc_bias, sram_language, format_time);
  const Directory dir;
  const BlockAlloc bat(size_mbits);

  std::memcpy(&card_data[BLOCK_SIZE * 0], &header, BLOCK_SIZE);
  std::memcpy(&card_data[BLOCK_SIZE * 1], &dir, BLOCK_SIZE);
  std::memcpy(&card_data[BLOCK_SIZE * 2], &dir, BLOCK_SIZE);
  std::memcpy(&card_data[BLOCK_SIZE * 3], &bat, BLOCK_SIZE);
  std::memcpy(&card_data[BLOCK_SIZE * 4], &bat, BLOCK_SIZE);

  return true;
}

/*************************************************************/
/* FZEROGX_MakeSaveGameValid                                 */
/* (use just before writing a F-Zero GX system .gci file)    */
/*                                                           */
/* Parameters:                                               */
/*    direntry:   [Description needed]                       */
/*    FileBuffer: [Description needed]                       */
/*                                                           */
/* Returns: Error code                                       */
/*************************************************************/

s32 GCMemcard::FZEROGX_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                         GCMBlock* FileBuffer, size_t num_blocks)
{
  u32 i, j;
  u16 chksum = 0xFFFF;

  // check for F-Zero GX system file
  if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "f_zero.dat") != 0)
    return 0;

  // also make sure that the filesize is correct
  if (num_blocks != 4)
    return 0;

  // get encrypted destination memory card serial numbers
  u32 serial1, serial2;
  cardheader.CalculateSerial(&serial1, &serial2);

  // set new serial numbers
  *(u16*)&FileBuffer[1].m_block[0x0066] = Common::swap16(u16(Common::swap32(serial1) >> 16));
  *(u16*)&FileBuffer[3].m_block[0x1580] = Common::swap16(u16(Common::swap32(serial2) >> 16));
  *(u16*)&FileBuffer[1].m_block[0x0060] = Common::swap16(u16(Common::swap32(serial1) & 0xFFFF));
  *(u16*)&FileBuffer[1].m_block[0x0200] = Common::swap16(u16(Common::swap32(serial2) & 0xFFFF));

  // calc 16-bit checksum
  for (i = 0x02; i < 0x8000; i++)
  {
    const int block = i / 0x2000;
    const int offset = i % 0x2000;
    chksum ^= (FileBuffer[block].m_block[offset] & 0xFF);
    for (j = 8; j > 0; j--)
    {
      if (chksum & 1)
        chksum = (chksum >> 1) ^ 0x8408;
      else
        chksum >>= 1;
    }
  }

  // set new checksum
  *(u16*)&FileBuffer[0].m_block[0x00] = Common::swap16(u16(~chksum));

  return 1;
}

/***********************************************************/
/* PSO_MakeSaveGameValid                                   */
/* (use just before writing a PSO system .gci file)        */
/*                                                         */
/* Parameters:                                             */
/*    direntry:   [Description needed]                     */
/*    FileBuffer: [Description needed]                     */
/*                                                         */
/* Returns: Error code                                     */
/***********************************************************/

s32 GCMemcard::PSO_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                     GCMBlock* FileBuffer, size_t num_blocks)
{
  u32 i, j;
  u32 chksum;
  u32 crc32LUT[256];
  u32 pso3offset = 0x00;

  // check for PSO1&2 system file
  if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "PSO_SYSTEM") != 0)
  {
    // check for PSO3 system file
    if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "PSO3_SYSTEM") == 0)
    {
      // PSO3 data block size adjustment
      pso3offset = 0x10;
    }
    else
    {
      // nothing to do
      return 0;
    }
  }

  // the system file's second block is changed
  if (num_blocks < 2)
    return 0;

  // get encrypted destination memory card serial numbers
  u32 serial1, serial2;
  cardheader.CalculateSerial(&serial1, &serial2);

  // set new serial numbers
  *(u32*)&FileBuffer[1].m_block[0x0158] = serial1;
  *(u32*)&FileBuffer[1].m_block[0x015C] = serial2;

  // generate crc32 LUT
  for (i = 0; i < 256; i++)
  {
    chksum = i;
    for (j = 8; j > 0; j--)
    {
      if (chksum & 1)
        chksum = (chksum >> 1) ^ 0xEDB88320;
      else
        chksum >>= 1;
    }

    crc32LUT[i] = chksum;
  }

  // PSO initial crc32 value
  chksum = 0xDEBB20E3;

  // calc 32-bit checksum
  for (i = 0x004C; i < 0x0164 + pso3offset; i++)
  {
    chksum = ((chksum >> 8) & 0xFFFFFF) ^ crc32LUT[(chksum ^ FileBuffer[1].m_block[i]) & 0xFF];
  }

  // set new checksum
  *(u32*)&FileBuffer[1].m_block[0x0048] = Common::swap32(chksum ^ 0xFFFFFFFF);

  return 1;
}

GCMBlock::GCMBlock()
{
  Erase();
}

void GCMBlock::Erase()
{
  memset(m_block.data(), 0xFF, m_block.size());
}

Header::Header()
{
  static_assert(std::is_trivially_copyable_v<Header>);
  std::memset(this, 0xFF, BLOCK_SIZE);
}

void InitializeHeaderData(HeaderData* data, const CardFlashId& flash_id, u16 size_mbits,
                          bool shift_jis, u32 rtc_bias, u32 sram_language, u64 format_time)
{
  // Nintendo format algorithm.
  // Constants are fixed by the GC SDK
  // Changing the constants will break memory card support
  data->m_size_mb = size_mbits;
  data->m_encoding = shift_jis ? 1 : 0;
  data->m_format_time = format_time;
  u64 rand = format_time;
  for (int i = 0; i < 12; i++)
  {
    rand = (((rand * (u64)0x0000000041c64e6dULL) + (u64)0x0000000000003039ULL) >> 16);
    data->m_serial[i] = (u8)(flash_id[i] + (u32)rand);
    rand = (((rand * (u64)0x0000000041c64e6dULL) + (u64)0x0000000000003039ULL) >> 16);
    rand &= (u64)0x0000000000007fffULL;
  }
  data->m_sram_bias = rtc_bias;
  data->m_sram_language = sram_language;
  // TODO: determine the purpose of m_dtv_status
  // 1 works for slot A, 0 works for both slot A and slot B
  data->m_dtv_status = 0;
  data->m_device_id = 0;
}

bool operator==(const HeaderData& lhs, const HeaderData& rhs)
{
  static_assert(std::is_trivially_copyable_v<HeaderData>);
  return std::memcmp(&lhs, &rhs, sizeof(HeaderData)) == 0;
}

Header::Header(const CardFlashId& flash_id, u16 size_mbits, bool shift_jis, u32 rtc_bias,
               u32 sram_language, u64 format_time)
{
  static_assert(std::is_trivially_copyable_v<Header>);
  std::memset(this, 0xFF, BLOCK_SIZE);
  InitializeHeaderData(&m_data, flash_id, size_mbits, shift_jis, rtc_bias, sram_language,
                       format_time);
  FixChecksums();
}

Header::Header(const HeaderData& data)
{
  static_assert(std::is_trivially_copyable_v<Header>);
  std::memset(this, 0xFF, BLOCK_SIZE);
  m_data = data;
  FixChecksums();
}

void Header::CalculateSerial(u32* serial1_out, u32* serial2_out) const
{
  static_assert(std::is_trivially_copyable<Header>());

  // the first 32 bytes, read in place
  const u8* const raw = reinterpret_cast<const u8*>(this);

  u32 serial1 = 0;
  u32 serial2 = 0;
  for (size_t i = 0; i < 32; i += 8)
  {
    serial1 ^= Common::BitCastPtr<u32>(&raw[i + 0]);
    serial2 ^= Common::BitCastPtr<u32>(&raw[i + 4]);
  }

  *serial1_out = serial1;
  *serial2_out = serial2;
}

DEntry::DEntry()
{
  memset(reinterpret_cast<u8*>(this), 0xFF, DENTRY_SIZE);
}

void Header::FixChecksums()
{
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  m_checksum = checksum_sum;
  m_checksum_inv = checksum_inv;
}

void Header::CalculateChecksums(u16* sum_out, u16* inv_out) const
{
  static_assert(std::is_trivially_copyable<Header>());

  // the checksummed bytes read in place
  constexpr size_t checksum_area_start = offsetof(Header, m_data);
  constexpr size_t checksum_area_end = offsetof(Header, m_checksum);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  CalculateMemcardChecksums(reinterpret_cast<const u8*>(this) + checksum_area_start,
                            checksum_area_size, sum_out, inv_out);
}

GCMemcardErrorCode Header::CheckForErrors(u16 card_size_mbits) const
{
  GCMemcardErrorCode error_code;

  // total card size should match card size in header
  if (m_data.m_size_mb != card_size_mbits)
    error_code.Set(GCMemcardValidityIssues::MISMATCHED_CARD_SIZE);

  // unused areas, should always be filled with 0xFF
  if (!IsFilledWithFF(m_unused_1.data(), m_unused_1.size()) ||
      !IsFilledWithFF(m_unused_2.data(), m_unused_2.size()))
  {
    error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
  }

  // verify checksums
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  return error_code;
}

bool Header::IsShiftJIS() const
{
  return m_data.m_encoding != 0;
}

Directory::Directory()
{
  memset(reinterpret_cast<u8*>(this), 0xFF, BLOCK_SIZE);
  m_update_counter = 0;
  m_checksum = Common::swap16(0xF003);
  m_checksum_inv = 0;
}

bool Directory::Replace(const DEntry& entry, size_t index)
{
  if (index >= m_dir_entries.size())
    return false;

  m_dir_entries[index] = entry;
  FixChecksums();
  return true;
}

void Directory::FixChecksums()
{
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  m_checksum = checksum_sum;
  m_checksum_inv = checksum_inv;
}

void Directory::CalculateChecksums(u16* sum_out, u16* inv_out) const
{
  static_assert(std::is_trivially_copyable<Directory>());

  // the checksummed bytes read in place
  constexpr size_t checksum_area_start = offsetof(Directory, m_dir_entries);
  constexpr size_t checksum_area_end = offsetof(Directory, m_checksum);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  CalculateMemcardChecksums(reinterpret_cast<const u8*>(this) + checksum_area_start,
                            checksum_area_size, sum_out, inv_out);
}

GCMemcardErrorCode Directory::CheckForErrors() const
{
  GCMemcardErrorCode error_code;

  // verify checksums
  u16 checksum_sum, checksum_inv;
  CalculateChecksums(&checksum_sum, &checksum_inv);
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  // unused area, should always be filled with 0xFF
  if (!IsFilledWithFF(m_padding.data(), m_padding.size()))
    error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);

  return error_code;
}

GCMemcardErrorCode Directory::CheckForErrorsWithBat(const BlockAlloc& bat) const
{
  GCMemcardErrorCode error_code;

  for (u8 i = 0; i < DIRLEN; ++i)
  {
    const DEntry& entry = m_dir_entries[i];
    if (entry.m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
      continue;

    // check if we end up with the same number of blocks when traversing through the BAT using the
    // given first block
    const u16 dir_number_of_blocks = entry.m_block_count;
    const u16 dir_first_block = entry.m_first_block;
    bool bat_block_count_matches = false;
    {
      u16 remaining_blocks = dir_number_of_blocks;
      u16 current_block = dir_first_block;
      while (true)
      {
        if (remaining_blocks == 0)
        {
          // we should be at the last block but haven't seen the last-block BAT indicator yet, file
          // is larger according to BAT, so we're inconsistent
          break;
        }
        --remaining_blocks;
        const u16 next_block = bat.GetNextBlock(current_block);
        if (next_block == 0)
        {
          // current block is out-of-range or next block is unallocated, this is definitely wrong
          break;
        }
        if (next_block == 0xFFFF)
        {
          // we're at the final block according to the BAT
          // if there are zero remaining blocks according to the directory we're consistent,
          // otherwise the file is smaller according to the BAT and we're inconsistent
          bat_block_count_matches = remaining_blocks == 0;
          break;
        }
        current_block = next_block;
      }
    }

    if (!bat_block_count_matches)
    {
      error_code.Set(GCMemcardValidityIssues::DIR_BAT_INCONSISTENT);
      break;
    }
  }

  // TODO: We could also check if every allocated BAT block is actually reachable with the files.

  return error_code;
}
}  // namespace Memcard
