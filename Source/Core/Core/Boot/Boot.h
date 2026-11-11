// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"

class PPCSymbolDB;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace File
{
class IOFile;
}

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace PowerPC
{
struct PowerPCState;
}

struct RegionSetting
{
  std::string area;
  std::string video;
  std::string game;
  std::string code;
};

class BootExecutableReader;

class BootSessionData
{
public:
  BootSessionData();
  BootSessionData(const BootSessionData& other) = delete;
  BootSessionData(BootSessionData&& other);
  BootSessionData& operator=(const BootSessionData& other) = delete;
  BootSessionData& operator=(BootSessionData&& other);
  ~BootSessionData();
};

struct BootParameters
{
  struct Disc
  {
    std::string path;
    std::unique_ptr<DiscIO::VolumeDisc> volume;
    std::vector<std::string> auto_disc_change_paths;
  };

  struct Executable
  {
    std::string path;
    std::unique_ptr<BootExecutableReader> reader;
  };

  struct NANDTitle
  {
    u64 id;
  };

  struct IPL
  {
    explicit IPL(DiscIO::Region region_);
    IPL(DiscIO::Region region_, Disc&& disc_);
    std::string path;
    DiscIO::Region region;
    // It is possible to boot the IPL with a disc inserted (with "skip IPL" disabled).
    std::optional<Disc> disc;
  };

  struct DFF
  {
    std::string dff_path;
  };

  static std::unique_ptr<BootParameters>
  GenerateFromFile(std::string boot_path, BootSessionData boot_session_data_ = BootSessionData());
  static std::unique_ptr<BootParameters>
  GenerateFromFile(std::vector<std::string> paths,
                   BootSessionData boot_session_data_ = BootSessionData());

  using Parameters = std::variant<Disc, Executable, DiscIO::VolumeWAD, NANDTitle, IPL, DFF>;
  BootParameters(Parameters&& parameters_, BootSessionData boot_session_data_ = BootSessionData());

  Parameters parameters;
  BootSessionData boot_session_data;
};

class CBoot
{
public:
  static bool BootUp(Core::System& system, const Core::CPUThreadGuard& guard,
                     std::unique_ptr<BootParameters> boot);

private:
  static bool DVDRead(Core::System& system, const DiscIO::VolumeDisc& disc, u64 dvd_offset,
                      u32 output_address, u32 length, const DiscIO::Partition& partition);
  static bool DVDReadDiscID(Core::System& system, const DiscIO::VolumeDisc& disc,
                            u32 output_address);
  static void RunFunction(Core::System& system, u32 address);

  static bool Boot_WiiWAD(Core::System& system, const DiscIO::VolumeWAD& wad);
  static bool BootNANDTitle(Core::System& system, u64 title_id);

  static void SetupMSR(Core::System& system);
  static void SetupHID(PowerPC::PowerPCState& ppc_state, bool is_wii);
  static void SetupBAT(Core::System& system, bool is_wii);
  static bool RunApploader(Core::System& system, const Core::CPUThreadGuard& guard, bool is_wii,
                           const DiscIO::VolumeDisc& volume);
  static bool EmulatedBS2_GC(Core::System& system, const Core::CPUThreadGuard& guard,
                             const DiscIO::VolumeDisc& volume);
  static bool EmulatedBS2_Wii(Core::System& system, const Core::CPUThreadGuard& guard,
                              const DiscIO::VolumeDisc& volume);
  static bool EmulatedBS2(Core::System& system, const Core::CPUThreadGuard& guard, bool is_wii,
                          const DiscIO::VolumeDisc& volume);
  static bool Load_BS2(Core::System& system, const std::string& boot_rom_filename);

  static void SetupGCMemory(Core::System& system, const Core::CPUThreadGuard& guard);
  static bool SetupWiiMemory(Core::System& system, IOS::HLE::IOSC::ConsoleType console_type);
};

class BootExecutableReader
{
public:
  // The executable lent from the file's mapping where it has one, else read into memory.
  explicit BootExecutableReader(const std::string& file_name);
  // The executable at data, lent by the caller, who keeps it alive and lets the reader
  // rewrite it in place; freed with free() by the reader when take_ownership.
  BootExecutableReader(u8* data, size_t size, bool take_ownership);
  virtual ~BootExecutableReader();

  BootExecutableReader(const BootExecutableReader&) = delete;
  BootExecutableReader& operator=(const BootExecutableReader&) = delete;

  virtual u32 GetEntryPoint() const = 0;
  virtual bool IsValid() const = 0;
  virtual bool IsWii() const = 0;
  virtual bool LoadIntoMemory(Core::System& system, bool only_in_mem1 = false) const = 0;
  virtual bool LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                           const std::string& filename) const = 0;

protected:
  // The executable's octets, read-only.
  const u8* m_bytes = nullptr;
  size_t m_size = 0;
  // The same octets, writable, copied out of a mapping on first use.
  u8* MutableBytes();

private:
  File::IOFile m_file;
  u8* m_mutable = nullptr;
  bool m_owned = false;
};

struct StateFlags
{
  void UpdateChecksum();
  u32 checksum;
  u8 flags;
  u8 type;
  u8 discstate;
  u8 returnto;
  u32 unknown[6];
};

// Reads the state file from the NAND, then calls the passed update function to update the struct,
// and finally writes the updated state file to the NAND.
void UpdateStateFlags(const std::function<void(StateFlags*)>& update_function);

/// Create title directories for the system menu (if needed).
///
/// Normally, this is automatically done by ES when the System Menu is installed,
/// but we cannot rely on this because we don't require any system titles to be installed.
void CreateSystemMenuTitleDirs();

std::vector<std::string> ReadM3UFile(const std::string& m3u_path, const std::string& folder_path);
