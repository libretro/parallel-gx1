// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCMemcard/GCIFile.h"

#include <cinttypes>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace Memcard
{
bool GCIFile::LoadHeader()
{
  if (!HasFilename())
    return false;

  File::IOFile save_file(m_filename, "rb");
  if (!save_file)
    return false;

  INFO_LOG(EXPANSIONINTERFACE, "Reading header from disk for %s", m_filename);
  if (!save_file.ReadBytes(&m_gci_header, sizeof(m_gci_header)))
  {
    ERROR_LOG(EXPANSIONINTERFACE, "Failed to read header for %s", m_filename);
    return false;
  }

  return true;
}

GCMBlock* GCIFile::SaveBlock(GCMBlock* pool, u32 index) const
{
  if (index >= m_num_used_blocks || m_used_blocks[index] >= GCI_MAX_BLOCKS)
    return nullptr;
  return &pool[m_used_blocks[index]];
}

bool GCIFile::LoadSaveBlocks(GCMBlock* pool)
{
  if (m_num_save_blocks)
    return true;
  if (!HasFilename())
    return false;

  File::IOFile save_file(m_filename, "rb");
  if (!save_file)
    return false;

  INFO_LOG(EXPANSIONINTERFACE, "Reading savedata from disk for %s", m_filename);
  const u16 num_blocks = m_gci_header.m_block_count;

  const u32 size = num_blocks * BLOCK_SIZE;
  const u64 file_size = save_file.GetSize();
  if (file_size != size + DENTRY_SIZE)
  {
    ERROR_LOG(EXPANSIONINTERFACE,
              "%s\nwas not loaded because it is an invalid GCI.\n File size (%#" PRIx64
              ") does not "
              "match the size recorded in the header (%#x)",
              m_filename, (u64)file_size, size + DENTRY_SIZE);
    return false;
  }

  save_file.Seek(DENTRY_SIZE, File::SeekOrigin::Begin);
  for (u32 i = 0; i < num_blocks; i++)
  {
    GCMBlock* const block = SaveBlock(pool, i);
    // a block past the ones it uses is read and dropped
    const bool ok = block ? save_file.ReadBytes(block, BLOCK_SIZE) :
                            save_file.Seek(BLOCK_SIZE, File::SeekOrigin::Current);
    if (!ok)
    {
      ERROR_LOG(EXPANSIONINTERFACE, "Failed to read data from GCI file %s", m_filename);
      return false;
    }
  }
  m_num_save_blocks = num_blocks;
  return true;
}

void GCIFile::EraseSaveBlocks(GCMBlock* pool)
{
  const u16 num_blocks = m_gci_header.m_block_count;
  for (u32 i = 0; i < num_blocks; i++)
  {
    GCMBlock* const block = SaveBlock(pool, i);
    if (block)
      block->Erase();
  }
  m_num_save_blocks = num_blocks;
}

bool GCIFile::HasCopyProtection() const
{
  if ((strcmp(reinterpret_cast<const char*>(m_gci_header.m_filename), "PSO_SYSTEM") == 0) ||
      (strcmp(reinterpret_cast<const char*>(m_gci_header.m_filename), "PSO3_SYSTEM") == 0) ||
      (strcmp(reinterpret_cast<const char*>(m_gci_header.m_filename), "f_zero.dat") == 0))
    return true;
  return false;
}

int GCIFile::UsesBlock(u16 block_num) const
{
  for (u16 i = 0; i < m_num_used_blocks; ++i)
  {
    if (m_used_blocks[i] == block_num)
      return i;
  }
  return -1;
}

/* As the std::string and std::vectors it had: each a u32 count, then the elements. The data
 * blocks come before the blocks they are kept at, so they are read where they lie in the state
 * and put in the pool once those are. */
void GCIFile::DoState(PointerWrap& p, GCMBlock* pool)
{
  u32 count, i;
  u8* data;

  p.Do(m_gci_header);
  p.Do(m_dirty);

  count = static_cast<u32>(strlen(m_filename));
  p.Do(count);
  if (count >= sizeof(m_filename))
  {
    p.SetMeasureMode();
    return;
  }
  p.DoArray(m_filename, count);
  m_filename[count] = '\0';

  count = m_num_save_blocks;
  p.Do(count);
  if (count > GCI_MAX_BLOCKS)
  {
    p.SetMeasureMode();
    return;
  }
  data = p.DoSkip(count * BLOCK_SIZE);
  if (data && p.IsWriteMode())
  {
    for (i = 0; i < count; i++)
    {
      const GCMBlock* const block = SaveBlock(pool, i);
      if (block)
        std::memcpy(data + i * BLOCK_SIZE, block, BLOCK_SIZE);
      else
        std::memset(data + i * BLOCK_SIZE, 0xFF, BLOCK_SIZE);
    }
  }
  const u32 num_save_blocks = count;

  count = m_num_used_blocks;
  p.Do(count);
  if (count > GCI_MAX_BLOCKS)
  {
    p.SetMeasureMode();
    return;
  }
  p.DoArray(m_used_blocks, count);

  if (p.IsReadMode())
  {
    m_num_used_blocks = static_cast<u16>(count);
    m_num_save_blocks = static_cast<u16>(num_save_blocks);
    for (i = 0; data && i < num_save_blocks; i++)
    {
      GCMBlock* const block = SaveBlock(pool, i);
      if (block)
        std::memcpy(block, data + i * BLOCK_SIZE, BLOCK_SIZE);
    }
  }
}
}  // namespace Memcard
