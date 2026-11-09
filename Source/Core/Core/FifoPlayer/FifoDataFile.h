// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/XFMemory.h"

/* The data of frames and memory updates is lent from the file's block, which the
 * FifoDataFile keeps: nothing is copied out of it to play. */
struct MemoryUpdate
{
  enum class Type : u8
  {
    TextureMap = 0x01,
    XFData = 0x02,
    VertexStream = 0x04,
    TMEM = 0x08,
  };

  u32 fifoPosition;
  u32 address;
  const u8* data;
  u32 dataSize;
  Type type;
};

struct FifoFrameInfo
{
  const u8* fifoData;
  u32 fifoDataSize;

  u32 fifoStart;
  u32 fifoEnd;

  // Must be sorted by fifoPosition
  const MemoryUpdate* memoryUpdates;
  u32 numMemoryUpdates;
};

class FifoDataFile
{
public:
  enum
  {
    BP_MEM_SIZE = 256,
    CP_MEM_SIZE = 256,
    XF_MEM_SIZE = 4096,
    XF_REGS_SIZE = 88,
    TEX_MEM_SIZE = 1024 * 1024,
  };
  static_assert((XF_MEM_SIZE + XF_REGS_SIZE) * sizeof(u32) == sizeof(XFMemory));

  FifoDataFile();
  ~FifoDataFile();

  FifoDataFile(const FifoDataFile&) = delete;
  FifoDataFile& operator=(const FifoDataFile&) = delete;

  bool GetIsWii() const;
  bool HasBrokenEFBCopies() const;
  static bool ShouldGenerateFakeVIUpdates();

  u32* GetBPMem() { return m_BPMem.data(); }
  u32* GetCPMem() { return m_CPMem.data(); }
  u32* GetXFMem() { return m_XFMem.data(); }
  u32* GetXFRegs() { return m_XFRegs.data(); }
  /* the file's texture memory, lent: TEX_MEM_SIZE bytes at most, GetTexMemSize of them */
  const u8* GetTexMem() const { return m_tex_mem; }
  u32 GetTexMemSize() const { return m_tex_mem_size; }
  u32 GetRamSizeReal() { return m_ram_size_real; }
  u32 GetExRamSizeReal() { return m_exram_size_real; }
  const std::string& GetGameId() const { return m_game_id; }

  const FifoFrameInfo& GetFrame(u32 frame) const { return m_frames[frame]; }
  u32 GetFrameCount() const { return m_num_frames; }

  static std::unique_ptr<FifoDataFile> Load(const std::string& filename, bool flagsOnly);

private:
  enum
  {
    FLAG_IS_WII = 1
  };

  bool GetFlag(u32 flag) const;

  std::array<u32, BP_MEM_SIZE> m_BPMem{};
  std::array<u32, CP_MEM_SIZE> m_CPMem{};
  std::array<u32, XF_MEM_SIZE> m_XFMem{};
  std::array<u32, XF_REGS_SIZE> m_XFRegs{};
  u32 m_ram_size_real = 0;
  u32 m_exram_size_real = 0;

  std::string m_game_id = std::string{DEFAULT_GAME_ID};

  u32 m_Flags = 0;
  u32 m_Version = 0;

  /* the whole file, read once: what the frames and the texture memory lend */
  u8* m_file = nullptr;
  const u8* m_tex_mem = nullptr;
  u32 m_tex_mem_size = 0;
  /* the frames, then every frame's memory updates, in one block */
  FifoFrameInfo* m_frames = nullptr;
  u32 m_num_frames = 0;
};
