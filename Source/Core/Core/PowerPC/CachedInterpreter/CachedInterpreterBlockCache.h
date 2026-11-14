// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

class CachedInterpreterBlockCache final : public JitBaseBlockCache
{
public:
  explicit CachedInterpreterBlockCache(JitBase& jit);

  void Init();

  void DestroyBlock(JitBlock& block);

  void ClearRangesToFree();

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFree() const
  {
    return m_ranges_to_free_on_next_codegen;
  }

  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest);
  void WriteDestroyBlock(const JitBlock& block);

private:
  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen;
};
