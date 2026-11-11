// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/FifoPlayer/FifoDataFile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

constexpr u32 FILE_ID = 0x0d01f1f0;
constexpr u32 VERSION_NUMBER = 6;

#pragma pack(push, 1)

struct FileHeader
{
  u32 fileId;
  u32 file_version;
  u32 min_loader_version;
  u64 bpMemOffset;
  u32 bpMemSize;
  u64 cpMemOffset;
  u32 cpMemSize;
  u64 xfMemOffset;
  u32 xfMemSize;
  u64 xfRegsOffset;
  u32 xfRegsSize;
  u64 frameListOffset;
  u32 frameCount;
  u32 flags;
  u64 texMemOffset;
  u32 texMemSize;
  // These are for overridden RAM sizes.  Otherwise the FIFO Player
  // will crash and burn with mismatched settings.  See PR #8722.
  u32 mem1_size;
  u32 mem2_size;
  char gameid[8];
  u8 reserved[24];
};
static_assert(sizeof(FileHeader) == 128, "FileHeader should be 128 bytes");
static_assert(DEFAULT_GAME_ID.size() == sizeof(FileHeader::gameid), "Default game id size changed");

struct FileFrameInfo
{
  u64 fifoDataOffset;
  u32 fifoDataSize;
  u32 fifoStart;
  u32 fifoEnd;
  u64 memoryUpdatesOffset;
  u32 numMemoryUpdates;
  u8 reserved[32];
};
static_assert(sizeof(FileFrameInfo) == 64, "FileFrameInfo should be 64 bytes");

struct FileMemoryUpdate
{
  u32 fifoPosition;
  u32 address;
  u64 dataOffset;
  u32 dataSize;
  u8 type;
  u8 reserved[3];
};
static_assert(sizeof(FileMemoryUpdate) == 24, "FileMemoryUpdate should be 24 bytes");

#pragma pack(pop)

FifoDataFile::FifoDataFile() = default;

FifoDataFile::~FifoDataFile()
{
  free(m_frames);
  free(m_file);
}

bool FifoDataFile::ShouldGenerateFakeVIUpdates()
{
  return true;
}

bool FifoDataFile::HasBrokenEFBCopies() const
{
  return m_Version < 2;
}

bool FifoDataFile::GetIsWii() const
{
  return GetFlag(FLAG_IS_WII);
}

bool FifoDataFile::GetFlag(u32 flag) const
{
  return !!(m_Flags & flag);
}

/* Whether [offset, offset + size) lies in a file of file_size bytes. */
static bool InFile(u64 offset, u64 size, u64 file_size)
{
  return offset <= file_size && size <= file_size - offset;
}

std::unique_ptr<FifoDataFile> FifoDataFile::Load(const std::string& filename, bool flagsOnly)
{
  File::IOFile file;
  file.Open(filename, "rb");
  if (!file)
    return nullptr;

  auto panic_failed_to_read = [] {
    CriticalAlert("Failed to read DFF file.");
    return nullptr;
  };

  if (file.GetSize() == 0)
  {
    CriticalAlert("DFF file size is 0; corrupt/incomplete file?");
    return nullptr;
  }

  FileHeader header;
  if (!file.ReadBytes(&header, sizeof(header)))
    return panic_failed_to_read();

  if (header.fileId != FILE_ID)
  {
    CriticalAlert("DFF file magic number is incorrect: got %08x, expected %08x", header.fileId,
                  FILE_ID);
    return nullptr;
  }

  if (header.min_loader_version > VERSION_NUMBER)
  {
    CriticalAlert(
        "The DFF's minimum loader version (%u) exceeds the version of this FIFO Player (%u)",
        header.min_loader_version, VERSION_NUMBER);
    return nullptr;
  }

  // Official support for overridden RAM sizes was added in version 5.
  if (header.file_version < 5)
  {
    // It's safe to assume FIFO Logs before PR #8722 weren't using this
    // obscure feature, so load up these header values with the old defaults.
    header.mem1_size = Memory::MEM1_SIZE_RETAIL;
    header.mem2_size = Memory::MEM2_SIZE_RETAIL;
  }

  auto dataFile = std::make_unique<FifoDataFile>();

  dataFile->m_Flags = header.flags;
  dataFile->m_Version = header.file_version;

  // Official support for game id was added in version 6
  if (header.file_version < 6)
  {
    dataFile->m_game_id = DEFAULT_GAME_ID;
  }
  else
  {
    const size_t gameid_length = strnlen(header.gameid, DEFAULT_GAME_ID.size());
    dataFile->m_game_id = std::string{header.gameid, gameid_length};
  }

  if (flagsOnly)
  {
    // Force settings to match those used when the DFF was created.  This is sort of a hack.
    // It only works because this function gets called twice, and the first time (flagsOnly mode)
    // happens to be before HW::Init().  But the convenience is hard to deny!
    Config::SetCurrent(Config::MAIN_RAM_OVERRIDE_ENABLE, true);
    Config::SetCurrent(Config::MAIN_MEM1_SIZE, header.mem1_size);
    Config::SetCurrent(Config::MAIN_MEM2_SIZE, header.mem2_size);

    return dataFile;
  }

  // To make up for such a hacky thing, here is a catch-all failsafe in case if the above code
  // stops working or is otherwise removed.  As it is, this should never end up running.
  // It should be noted, however, that Dolphin *will still crash* from the nullptr being returned
  // in a non-flagsOnly context, so if this code becomes necessary, it should be moved above the
  // prior conditional.
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  if (header.mem1_size != memory.GetRamSizeReal() || header.mem2_size != memory.GetExRamSizeReal())
  {
    CriticalAlert("Emulated memory size mismatch!\n"
                  "Current: MEM1 %08X (%u MiB), MEM2 %08X (%u MiB)\n"
                  "DFF: MEM1 %08X (%u MiB), MEM2 %08X (%u MiB)",
                  memory.GetRamSizeReal(), memory.GetRamSizeReal() / 0x100000,
                  memory.GetExRamSizeReal(), memory.GetExRamSizeReal() / 0x100000, header.mem1_size,
                  header.mem1_size / 0x100000, header.mem2_size, header.mem2_size / 0x100000);
    return nullptr;
  }

  /* the rest comes from the whole file, read once: frames and updates lend from it */
  const u64 file_size = file.GetSize();
  dataFile->m_file = static_cast<u8*>(malloc(file_size ? static_cast<size_t>(file_size) : 1));
  if (!dataFile->m_file || !file.Seek(0, File::SeekOrigin::Begin) ||
      !file.ReadBytes(dataFile->m_file, static_cast<size_t>(file_size)))
  {
    return panic_failed_to_read();
  }
  const u8* const data = dataFile->m_file;

  /* a register array: what the file has of it, the rest zero */
  auto read_regs = [&](u32* regs, u32 count, u64 offset, u32 file_count) {
    const u32 n = std::min(count, file_count);
    if (!InFile(offset, u64{n} * sizeof(u32), file_size))
      return false;
    std::memcpy(regs, data + offset, n * sizeof(u32));
    return true;
  };
  if (!read_regs(dataFile->m_BPMem.data(), BP_MEM_SIZE, header.bpMemOffset, header.bpMemSize) ||
      !read_regs(dataFile->m_CPMem.data(), CP_MEM_SIZE, header.cpMemOffset, header.cpMemSize) ||
      !read_regs(dataFile->m_XFMem.data(), XF_MEM_SIZE, header.xfMemOffset, header.xfMemSize) ||
      !read_regs(dataFile->m_XFRegs.data(), XF_REGS_SIZE, header.xfRegsOffset,
                 header.xfRegsSize))
  {
    return panic_failed_to_read();
  }

  // Texture memory saving was added in version 4.
  if (dataFile->m_Version >= 4)
  {
    const u32 size = std::min<u32>(TEX_MEM_SIZE, header.texMemSize);
    if (!InFile(header.texMemOffset, size, file_size))
      return panic_failed_to_read();
    dataFile->m_tex_mem = data + header.texMemOffset;
    dataFile->m_tex_mem_size = size;
  }

  // idk what else these could be used for, but it'd be a shame to not make them available.
  dataFile->m_ram_size_real = header.mem1_size;
  dataFile->m_exram_size_real = header.mem2_size;

  // The frames, and the number of memory updates of them all, for one block
  if (!InFile(header.frameListOffset, u64{header.frameCount} * sizeof(FileFrameInfo), file_size))
    return panic_failed_to_read();
  u64 num_updates = 0;
  for (u32 i = 0; i < header.frameCount; ++i)
  {
    FileFrameInfo src;
    std::memcpy(&src, data + header.frameListOffset + u64{i} * sizeof(FileFrameInfo), sizeof(src));
    if (!InFile(src.fifoDataOffset, src.fifoDataSize, file_size) ||
        !InFile(src.memoryUpdatesOffset, u64{src.numMemoryUpdates} * sizeof(FileMemoryUpdate),
                file_size))
    {
      return panic_failed_to_read();
    }
    num_updates += src.numMemoryUpdates;
  }

  dataFile->m_frames = static_cast<FifoFrameInfo*>(
      malloc(header.frameCount * sizeof(FifoFrameInfo) + num_updates * sizeof(MemoryUpdate) + 1));
  if (!dataFile->m_frames)
    return panic_failed_to_read();
  MemoryUpdate* updates = reinterpret_cast<MemoryUpdate*>(dataFile->m_frames + header.frameCount);

  for (u32 i = 0; i < header.frameCount; ++i)
  {
    FileFrameInfo src;
    std::memcpy(&src, data + header.frameListOffset + u64{i} * sizeof(FileFrameInfo), sizeof(src));
    FifoFrameInfo& dst = dataFile->m_frames[i];
    dst.fifoData = data + src.fifoDataOffset;
    dst.fifoDataSize = src.fifoDataSize;
    dst.fifoStart = src.fifoStart;
    dst.fifoEnd = src.fifoEnd;
    dst.memoryUpdates = updates;
    dst.numMemoryUpdates = src.numMemoryUpdates;

    for (u32 j = 0; j < src.numMemoryUpdates; ++j)
    {
      FileMemoryUpdate src_update;
      std::memcpy(&src_update,
                  data + src.memoryUpdatesOffset + u64{j} * sizeof(FileMemoryUpdate),
                  sizeof(src_update));
      if (!InFile(src_update.dataOffset, src_update.dataSize, file_size))
        return panic_failed_to_read();
      MemoryUpdate& dst_update = *updates++;
      dst_update.fifoPosition = src_update.fifoPosition;
      dst_update.address = src_update.address;
      dst_update.data = data + src_update.dataOffset;
      dst_update.dataSize = src_update.dataSize;
      dst_update.type = static_cast<MemoryUpdate::Type>(src_update.type);
    }
    dataFile->m_num_frames = i + 1;
  }

  return dataFile;
}
