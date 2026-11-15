// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Jit64Common/BlockCache.h"

#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

JitBlockCache::JitBlockCache(JitBase& jit) : JitBaseBlockCache{jit}
{
  code_range_list_init(&m_ranges_to_free_on_next_codegen_near);
  code_range_list_init(&m_ranges_to_free_on_next_codegen_far);
}

JitBlockCache::~JitBlockCache()
{
  code_range_list_free(&m_ranges_to_free_on_next_codegen_near);
  code_range_list_free(&m_ranges_to_free_on_next_codegen_far);
}

void JitBlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  u8* location = source.exitPtrs;
  const u8* address = dest ? dest->normalEntry : m_jit.GetAsmRoutines()->dispatcher_no_timing_check;
  if (source.call)
  {
    Gen::XEmitter emit(location, location + 5);
    emit.CALL(address);
  }
  else
  {
    // If we're going to link with the next block, there is no need
    // to emit JMP. So just NOP out the gap to the next block.
    // Support up to 3 additional bytes because of alignment.
    s64 offset = address - location;
    if (offset > 0 && offset <= Gen::XEmitter::NEAR_JMP_LEN + 3)
    {
      Gen::XEmitter emit(location, location + offset);
      emit.NOP(offset);
    }
    else
    {
      // Length forced to Near because JMP length might differ between dispatcher and linked block.
      // Technically this isn't necessary, as this is executed after Jit64::JustWriteExit (which
      // also pads), and a Short JMP written on top of a Near JMP isn't incorrect since the garbage
      // bytes are skipped. But they confuse the disassembler, and probably the CPU speculation too.
      Gen::XEmitter emit(location, location + Gen::XEmitter::NEAR_JMP_LEN);
      emit.JMP(address, true);
    }
  }
}

void JitBlockCache::WriteDestroyBlock(const JitBlock& block)
{
  // Only clear the entry point as we might still be within this block.
  Gen::XEmitter emit(block.normalEntry, block.normalEntry + 1);
  emit.INT3();
}

// A block's code is longer than a near JMP, and nothing jumps into the first instruction it
// emits: at least as long as a CALL, or padded for fastmem to as long as the JMP of a backpatch.
// Its exits set the PC before they jump, so whatever comes in goes to the dispatcher closed.
void JitBlockCache::WriteCloseEntry(JitBlock& block)
{
  std::memcpy(block.entry_code, block.normalEntry, Gen::XEmitter::NEAR_JMP_LEN);
  Gen::XEmitter emit(block.normalEntry, block.normalEntry + Gen::XEmitter::NEAR_JMP_LEN);
  emit.JMP(m_jit.GetAsmRoutines()->dispatcher_no_timing_check, true);
}

void JitBlockCache::WriteOpenEntry(JitBlock& block)
{
  std::memcpy(block.normalEntry, block.entry_code, Gen::XEmitter::NEAR_JMP_LEN);
}

void JitBlockCache::Init()
{
  JitBaseBlockCache::Init();
  ClearRangesToFree();
}

void JitBlockCache::DestroyBlock(JitBlock& block)
{
  JitBaseBlockCache::DestroyBlockCommon(block);

  if (block.near_begin != block.near_end)
    code_range_list_push(&m_ranges_to_free_on_next_codegen_near, block.near_begin, block.near_end);
  if (block.far_begin != block.far_end)
    code_range_list_push(&m_ranges_to_free_on_next_codegen_far, block.far_begin, block.far_end);
}

const CodeRangeList& JitBlockCache::GetRangesToFreeNear() const
{
  return m_ranges_to_free_on_next_codegen_near;
}

const CodeRangeList& JitBlockCache::GetRangesToFreeFar() const
{
  return m_ranges_to_free_on_next_codegen_far;
}

void JitBlockCache::ClearRangesToFree()
{
  m_ranges_to_free_on_next_codegen_near.count = 0;
  m_ranges_to_free_on_next_codegen_far.count = 0;
}
