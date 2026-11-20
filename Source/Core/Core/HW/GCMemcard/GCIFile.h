// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <retro_miscellaneous.h>

#include "Common/CommonTypes.h"
#include "Core/HW/GCMemcard/GCMemcard.h"

class PointerWrap;

namespace Memcard
{
// The most blocks of a card, and so of a save on it (MBIT_SIZE_MEMORY_CARD_2043).
constexpr u32 GCI_MAX_BLOCKS = MBIT_SIZE_MEMORY_CARD_2043 * MBIT_TO_BLOCKS;

// A save of a GCI folder card. Its data is kept in the card's blocks, a pool of GCI_MAX_BLOCKS
// indexed by card block, at the blocks it uses, so nothing is allocated for it.
class GCIFile
{
public:
  bool LoadHeader();
  // Its data from its file, to the blocks it uses in the pool.
  bool LoadSaveBlocks(GCMBlock* pool);
  // Its data, as a new save's: every block it uses erased.
  void EraseSaveBlocks(GCMBlock* pool);
  bool HasCopyProtection() const;
  void DoState(PointerWrap& p, GCMBlock* pool);
  int UsesBlock(u16 blocknum) const;
  // The data block of the save at the index, in the pool; nullptr when it uses no block there.
  GCMBlock* SaveBlock(GCMBlock* pool, u32 index) const;
  bool HasFilename() const { return m_filename[0] != '\0'; }

  DEntry m_gci_header;
  // the blocks of data it has in memory, none until read or written
  u16 m_num_save_blocks = 0;
  u16 m_num_used_blocks = 0;
  u16 m_used_blocks[GCI_MAX_BLOCKS];
  bool m_dirty = false;
  char m_filename[PATH_MAX_LENGTH] = {};
};
}  // namespace Memcard
