// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

class JitBlockCache final : public JitBaseBlockCache
{
public:
  explicit JitBlockCache(JitBase& jit);
  ~JitBlockCache();

  void Init();

  void DestroyBlock(JitBlock& block);

  const CodeRangeList& GetRangesToFreeNear() const;
  const CodeRangeList& GetRangesToFreeFar() const;

  void ClearRangesToFree();

  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest);
  void WriteDestroyBlock(const JitBlock& block);
  void WriteCloseEntry(JitBlock& block);
  void WriteOpenEntry(JitBlock& block);

private:
  CodeRangeList m_ranges_to_free_on_next_codegen_near;
  CodeRangeList m_ranges_to_free_on_next_codegen_far;
};
