// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Arm64Emitter.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

typedef void (*CompiledCode)();

class JitArm64BlockCache final : public JitBaseBlockCache
{
public:
  explicit JitArm64BlockCache(JitBase& jit);
  ~JitArm64BlockCache();

  void Init();

  void DestroyBlock(JitBlock& block);

  const CodeRangeList& GetRangesToFreeNear() const;
  const CodeRangeList& GetRangesToFreeFar() const;

  void ClearRangesToFree();

  void WriteLinkBlock(Arm64Gen::ARM64XEmitter& emit, const JitBlock::LinkData& source,
                      const JitBlock* dest = nullptr);

  static constexpr size_t BLOCK_LINK_SIZE = 3 * sizeof(u32);
  static constexpr size_t BLOCK_LINK_FAST_BL_OFFSET = BLOCK_LINK_SIZE - sizeof(u32);

  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest);
  void WriteDestroyBlock(const JitBlock& block);

private:
  CodeRangeList m_ranges_to_free_on_next_codegen_near;
  CodeRangeList m_ranges_to_free_on_next_codegen_far;
};
