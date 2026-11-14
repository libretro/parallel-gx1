// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

class JitBlockCache final : public JitBaseBlockCache
{
public:
  explicit JitBlockCache(JitBase& jit);

  void Init();

  void DestroyBlock(JitBlock& block);

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeNear() const;
  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeFar() const;

  void ClearRangesToFree();

  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest);
  void WriteDestroyBlock(const JitBlock& block);

private:
  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen_near;
  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen_far;
};
