// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Boot/Boot.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Boot/DolReader.h"
#include "Core/Boot/ElfReader.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"

static void PushLine(void* ud, const char* text, size_t length)
{
  static_cast<std::vector<std::string>*>(ud)->emplace_back(text, length);
}

std::vector<std::string> ReadM3UFile(const std::string& m3u_path, const std::string& folder_path)
{
  std::vector<std::string> result;
  std::vector<std::string> lines;
  /* the paths not found, a line each, cut when they run past it */
  char missing[1024];
  size_t missing_len = 0;

  File::ForEachLine(m3u_path, PushLine, &lines);

  for (std::string& line : lines)
  {
    // This is the UTF-8 representation of U+FEFF.
    constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

    if (line.starts_with(utf8_bom))
    {
      WARN_LOG(BOOT, "UTF-8 BOM in file: %s", m3u_path.c_str());
      line.erase(0, utf8_bom.length());
    }

    if (!line.empty() && line.front() != '#')  // Comments start with #
    {
      std::string path_to_add = File::JoinPath(folder_path, line);
      if (File::Exists(path_to_add))
      {
        result.push_back(std::move(path_to_add));
      }
      else if (missing_len < sizeof(missing))
      {
        const int n = snprintf(missing + missing_len, sizeof(missing) - missing_len, "%s%s",
                               missing_len ? "\n" : "", path_to_add.c_str());
        missing_len = n < 0 ? sizeof(missing) : missing_len + static_cast<size_t>(n);
      }
    }
  }

  if (missing_len)
  {
    PanicAlert("Files specified in the M3U file \"%s\" were not found:\n%s", m3u_path.c_str(),
               missing);
    return {};
  }

  if (result.empty())
    PanicAlert("No paths found in the M3U file \"%s\"", m3u_path.c_str());

  return result;
}

BootSessionData::BootSessionData()
{
}

BootSessionData::BootSessionData(BootSessionData&& other) = default;

BootSessionData& BootSessionData::operator=(BootSessionData&& other) = default;

BootSessionData::~BootSessionData() = default;

BootParameters::BootParameters(Parameters&& parameters_, BootSessionData boot_session_data_)
    : parameters(std::move(parameters_)), boot_session_data(std::move(boot_session_data_))
{
}

std::unique_ptr<BootParameters> BootParameters::GenerateFromFile(std::string boot_path,
                                                                 BootSessionData boot_session_data_)
{
  return GenerateFromFile(std::vector<std::string>{std::move(boot_path)},
                          std::move(boot_session_data_));
}

std::unique_ptr<BootParameters> BootParameters::GenerateFromFile(std::vector<std::string> paths,
                                                                 BootSessionData boot_session_data_)
{
  ASSERT(!paths.empty());

  for (std::string& path : paths)
    UnifyPathSeparators(path);

  // Check if the file exist, we may have gotten it from a --elf command line
  // that gave an incorrect file name
  if (!File::Exists(paths.front()))
  {
    PanicAlert("The specified file \"%s\" does not exist", paths.front().c_str());
    return {};
  }

  std::string extension;
  SplitPath(paths.front(),
            nullptr,
            nullptr, &extension);
  Common::ToLower(&extension);

  std::string path = paths.front();
  if (paths.size() == 1)
    paths.clear();

  static const std::unordered_set<std::string> disc_image_extensions = {
      {".gcm", ".bin", ".iso", ".tgc", ".wbfs", ".ciso", ".gcz", ".wia", ".rvz", ".nfs", ".dol",
       ".elf"}};
  if (disc_image_extensions.contains(extension))
  {
    DiscIO::VolumeDisc* disc = DiscIO::CreateDiscForCore(path);
    if (disc)
    {
      return std::make_unique<BootParameters>(Disc{std::move(path), disc, paths},
                                              std::move(boot_session_data_));
    }

    if (extension == ".elf")
    {
      auto elf_reader = std::make_unique<ElfReader>(path);
      return std::make_unique<BootParameters>(Executable{std::move(path), std::move(elf_reader)},
                                              std::move(boot_session_data_));
    }

    if (extension == ".dol")
    {
      auto dol_reader = std::make_unique<DolReader>(path);
      return std::make_unique<BootParameters>(Executable{std::move(path), std::move(dol_reader)},
                                              std::move(boot_session_data_));
    }

    PanicAlert("\"%s\" is an invalid GCM/ISO file, or is not a GC/Wii ISO.", path.c_str());
    return {};
  }

  if (extension == ".dff")
    return std::make_unique<BootParameters>(DFF{std::move(path)}, std::move(boot_session_data_));

  if (extension == ".wad")
  {
    DiscIO::VolumeWAD* wad = DiscIO::CreateWAD(path);
    if (wad)
    {
      // the WAD moves into the parameters, its reader with it; what is left of it goes
      auto parameters =
          std::make_unique<BootParameters>(std::move(*wad), std::move(boot_session_data_));
      DiscIO::DestroyVolume(wad);
      return parameters;
    }
  }

  PanicAlert("Could not recognize file %s", path.c_str());
  return {};
}

BootParameters::Disc::Disc(std::string path_, DiscIO::VolumeDisc* volume_,
                           std::vector<std::string> auto_disc_change_paths_)
    : path(std::move(path_)), volume(volume_),
      auto_disc_change_paths(std::move(auto_disc_change_paths_))
{
}

BootParameters::Disc::Disc(Disc&& other) noexcept
    : path(std::move(other.path)), volume(other.volume),
      auto_disc_change_paths(std::move(other.auto_disc_change_paths))
{
  other.volume = nullptr;
}

BootParameters::Disc& BootParameters::Disc::operator=(Disc&& other) noexcept
{
  if (this != &other)
  {
    DiscIO::DestroyVolume(volume);
    path = std::move(other.path);
    volume = other.volume;
    other.volume = nullptr;
    auto_disc_change_paths = std::move(other.auto_disc_change_paths);
  }
  return *this;
}

BootParameters::Disc::~Disc()
{
  DiscIO::DestroyVolume(volume);
}

BootParameters::IPL::IPL(DiscIO::Region region_) : region(region_)
{
  const std::string directory = Config::GetDirectoryForRegion(region);
  path = Config::GetBootROMPath(directory);
}

BootParameters::IPL::IPL(DiscIO::Region region_, Disc&& disc_) : IPL(region_)
{
  disc = std::move(disc_);
}

// Inserts a disc into the emulated disc drive and returns a pointer to it.
// The returned pointer must only be used while we are still booting,
// because DVDThread can do whatever it wants to the disc after that.
// Takes disc.
static const DiscIO::VolumeDisc* SetDisc(DVD::DVDInterface& dvd_interface, DiscIO::VolumeDisc* disc,
                                         std::vector<std::string> auto_disc_change_paths = {})
{
  dvd_interface.SetDisc(disc, auto_disc_change_paths);
  return disc;
}

bool CBoot::DVDRead(Core::System& system, const DiscIO::VolumeDisc& disc, u64 dvd_offset,
                    u32 output_address, u32 length, const DiscIO::Partition& partition)
{
  std::vector<u8> buffer(length);
  if (!disc.Read(dvd_offset, length, buffer.data(), partition))
    return false;

  auto& memory = system.GetMemory();
  memory.CopyToEmu(output_address, buffer.data(), length);
  return true;
}

bool CBoot::DVDReadDiscID(Core::System& system, const DiscIO::VolumeDisc& disc, u32 output_address)
{
  std::array<u8, 0x20> buffer;
  if (!disc.Read(0, buffer.size(), buffer.data(), DiscIO::PARTITION_NONE))
    return false;

  auto& memory = system.GetMemory();
  memory.CopyToEmu(output_address, buffer.data(), buffer.size());

  // Transition out of the DiscIdNotRead state (which the drive should be in at this point,
  // on the assumption that this is only used for the first read)
  system.GetDVDInterface().SetDriveState(DVD::DriveState::ReadyNoReadsMade);
  return true;
}

// If ipl.bin is not found, this function does *some* of what BS1 does:
// loading IPL(BS2) and jumping to it.
// It does not initialize the hardware or anything else like BS1 does.
bool CBoot::Load_BS2(Core::System& system, const std::string& boot_rom_filename)
{
  // CRC32 hashes of the IPL file, obtained from Redump
  // DOL-001(USA) / DOL-001(JPN) / DOT-001 / SL-GC10 (NTSC Revision 1.0)
  constexpr u32 NTSC_v1_0 = 0x6DAC1F2A;
  // DOL-001(USA) / DOL-001(JPN) / DOT-001 (NTSC Revision 1.1)
  constexpr u32 NTSC_v1_1 = 0xD5E6FEEA;
  // DOL-001(USA) / DOL-001(JPN) (NTSC Revision 1.2)
  constexpr u32 NTSC_v1_2_001 = 0xD235E3F9;
  // DOL-101(USA) / DOL-101(JPN) (NTSC Revision 1.2)
  constexpr u32 NTSC_v1_2_101 = 0x86573808;
  // DOL-002(BRA) (MPAL Revision 1.1)
  constexpr u32 MPAL_v1_1 = 0x667D0B64;
  // DOL-001(EUR) / DOT-001P (PAL Revision 1.0)
  constexpr u32 PAL_v1_0 = 0x4F319F43;
  // DOL-101(EUR) (PAL Revision 1.2)
  constexpr u32 PAL_v1_2 = 0xAD1B7F16;
  // Triforce Arcade IPL (DEV Revision 1.0)
  constexpr u32 Triforce = 0xD1883221;

  // Load the IPL ROM dump, limited to 2MiB which is the size of the official IPLs.
  constexpr size_t max_ipl_size = 2 * 1024 * 1024;
  std::vector<u8> data;
  {
    File::IOFile file(boot_rom_filename, "rb");
    if (!file)
      return false;

    data.resize(static_cast<size_t>(std::min<u64>(file.GetSize(), max_ipl_size)));
    if (!file.ReadArray(data.data(), data.size()))
      return false;
  }

  const u32 ipl_hash = common_crc32_compute(data.data(), data.size());
  bool known_ipl = false;
  bool pal_ipl = false;
  switch (ipl_hash)
  {
  case NTSC_v1_0:
  case NTSC_v1_1:
  case NTSC_v1_2_001:
  case NTSC_v1_2_101:
  case MPAL_v1_1:
    known_ipl = true;
    break;
  case PAL_v1_0:
  case PAL_v1_2:
    pal_ipl = true;
    known_ipl = true;
    break;
  case Triforce:
    known_ipl = true;
    break;
  default:
    PanicAlert("The IPL file is not a known good dump. (CRC32: %x)", ipl_hash);
    break;
  }

  const DiscIO::Region boot_region = SConfig::GetInstance().m_region;
  if (known_ipl && pal_ipl != (boot_region == DiscIO::Region::PAL))
  {
    PanicAlert("%s IPL found in %s directory. The disc might not be recognized",
               pal_ipl ? "PAL" : "NTSC", Config::GetDirectoryForRegion(boot_region));
  }

  // Run the descrambler over the encrypted section containing BS1/BS2
  if (data.size() > 0x100)
  {
    ExpansionInterface::CEXIIPL::Descrambler(
        data.data() + 0x100, static_cast<u32>(std::min<size_t>(data.size() - 0x100, 0x1AFE00)));
  }

  // TODO: Execution is supposed to start at 0xFFF00000, not 0x81200000;
  // copying the initial boot code to 0x81200000 is a hack.
  // For now, HLE the first few instructions and start at 0x81200150
  // to work around this.
  auto& memory = system.GetMemory();
  if (data.size() > 0x100)
  {
    memory.CopyToEmu(0x01200000, data.data() + 0x100, std::min<size_t>(data.size() - 0x100, 0x700));
  }
  if (data.size() > 0x820)
  {
    memory.CopyToEmu(0x01300000, data.data() + 0x820,
                     std::min<size_t>(data.size() - 0x820, 0x1AFE00));
  }

  auto& ppc_state = system.GetPPCState();
  ppc_state.gpr[3] = 0xfff0001f;
  ppc_state.gpr[4] = 0x00002030;
  ppc_state.gpr[5] = 0x0000009c;

  ppc_state.msr.FP = 1;
  ppc_state.msr.DR = 1;
  ppc_state.msr.IR = 1;

  ppc_state.spr[SPR_HID0] = 0x0011c464;
  ppc_state.spr[SPR_IBAT3U] = 0xfff0001f;
  ppc_state.spr[SPR_IBAT3L] = 0xfff00001;
  ppc_state.spr[SPR_DBAT3U] = 0xfff0001f;
  ppc_state.spr[SPR_DBAT3L] = 0xfff00001;
  SetupBAT(system, /*is_wii*/ false);

  if (ipl_hash == Triforce)
  {
    HLE::Patch(system, 0x813048B8, "OSReport");
    HLE::Patch(system, 0x8130095C, "OSReport");  // Apploader

    AMMediaboard::FirmwareMap(true);
  }

  ppc_state.pc = 0x81200150;

  system.GetPowerPC().MSRUpdated();

  return true;
}

static void SetDefaultDisc(DVD::DVDInterface& dvd_interface)
{
  const std::string default_iso = Config::Get(Config::MAIN_DEFAULT_ISO);
  if (!default_iso.empty())
    SetDisc(dvd_interface, DiscIO::CreateDiscForCore(default_iso));
}

static void CopyDefaultExceptionHandlers(Core::System& system)
{
  constexpr u32 EXCEPTION_HANDLER_ADDRESSES[] = {0x00000100, 0x00000200, 0x00000300, 0x00000400,
                                                 0x00000500, 0x00000600, 0x00000700, 0x00000800,
                                                 0x00000900, 0x00000C00, 0x00000D00, 0x00000F00,
                                                 0x00001300, 0x00001400, 0x00001700};

  auto& memory = system.GetMemory();
  constexpr u32 RFI_INSTRUCTION = 0x4C000064;
  for (const u32 address : EXCEPTION_HANDLER_ADDRESSES)
    memory.Write_U32(RFI_INSTRUCTION, address);
}

// Third boot step after BootManager and Core. See Call schedule in BootManager.cpp
bool CBoot::BootUp(Core::System& system, const Core::CPUThreadGuard& guard,
                   std::unique_ptr<BootParameters> boot)
{
  SConfig& config = SConfig::GetInstance();

  // Triforce systems are region free and always must use the NTSC video mode
  if (system.IsTriforce())
  {
    config.m_region = DiscIO::Region::NTSC_J;
  }

  // PAL Wii uses NTSC framerate and linecount in 60Hz modes
  system.GetVideoInterface().Preset(DiscIO::IsNTSC(config.m_region) ||
                                    (system.IsWii() && Config::Get(Config::SYSCONF_PAL60)));

  struct BootTitle
  {
    BootTitle(Core::System& system_, const Core::CPUThreadGuard& guard_)
        : system(system_), guard(guard_), config(SConfig::GetInstance())
    {
    }
    bool operator()(BootParameters::Disc& disc) const
    {
      NOTICE_LOG(BOOT, "Booting from disc: %s", disc.path.c_str());
      DiscIO::VolumeDisc* const taken = disc.volume;
      disc.volume = nullptr;
      const DiscIO::VolumeDisc* volume =
          SetDisc(system.GetDVDInterface(), taken, disc.auto_disc_change_paths);

      if (!volume)
        return false;

      if (!EmulatedBS2(system, guard, system.IsWii(), *volume))
        return false;

      SConfig::OnTitleDirectlyBooted(guard);
      return true;
    }

    bool operator()(const BootParameters::Executable& executable) const
    {
      NOTICE_LOG(BOOT, "Booting from executable: %s", executable.path.c_str());

      if (!executable.reader->IsValid())
        return false;

      SetDefaultDisc(system.GetDVDInterface());

      auto& ppc_state = system.GetPPCState();

      SetupMSR(system);
      SetupHID(ppc_state, system.IsWii());
      SetupBAT(system, system.IsWii());
      CopyDefaultExceptionHandlers(system);

      if (system.IsWii())
      {
        // Set a value for the SP. It doesn't matter where this points to,
        // as long as it is a valid location. This value is taken from a homebrew binary.
        ppc_state.gpr[1] = 0x8004d4bc;

        // Because there is no TMD to get the requested system (IOS) version from,
        // we default to IOS58, which is the version used by the Homebrew Channel.
        SetupWiiMemory(system, IOS::HLE::IOSC::ConsoleType::Retail);
        system.GetIOS()->BootIOS(Titles::IOS(58));

        // The Apploader writes an IOS-like version number into memory.
        // Older versions of OSInit read it to check IOS compatibility.
        constexpr u32 ADDR_IOS_VERSION = 0x3140;
        constexpr u32 ADDR_APPLOADER_VERSION = 0x3188;
        const u32 ios_version = system.GetMemory().Read_U32(ADDR_IOS_VERSION);
        system.GetMemory().Write_U32(ios_version, ADDR_APPLOADER_VERSION);
      }
      else
      {
        SetupGCMemory(system, guard);
      }

      if (!executable.reader->LoadIntoMemory(system))
      {
        PanicAlert("Failed to load the executable to memory.");
        return false;
      }


      SConfig::OnTitleDirectlyBooted(guard);

      ppc_state.pc = executable.reader->GetEntryPoint();

      const std::string filename = PathToFileName(executable.path);

      auto& ppc_symbol_db = system.GetPPCSymbolDB();
      ppc_symbol_db.Clear();

      if (executable.reader->LoadSymbols(guard, ppc_symbol_db, filename))
        HLE::PatchFunctions(system);

      return true;
    }

    bool operator()(const DiscIO::VolumeWAD& wad) const
    {
      SetDefaultDisc(system.GetDVDInterface());
      if (!Boot_WiiWAD(system, wad))
        return false;


      SConfig::OnTitleDirectlyBooted(guard);
      return true;
    }

    bool operator()(const BootParameters::NANDTitle& nand_title) const
    {
      SetDefaultDisc(system.GetDVDInterface());
      if (!BootNANDTitle(system, nand_title.id))
        return false;


      SConfig::OnTitleDirectlyBooted(guard);
      return true;
    }

    bool operator()(const BootParameters::IPL& ipl) const
    {
      NOTICE_LOG(BOOT, "Booting GC IPL: %s", ipl.path.c_str());
      if (!File::Exists(ipl.path))
      {
        if (ipl.disc)
          PanicAlert("Cannot start the game, because the GC IPL could not be found.");
        else
          PanicAlert("Cannot find the GC IPL.");
        return false;
      }

      if (!Load_BS2(system, ipl.path))
        return false;

      if (ipl.disc)
      {
        NOTICE_LOG(BOOT, "Inserting disc: %s", ipl.disc->path.c_str());

        SetDisc(system.GetDVDInterface(), DiscIO::CreateDiscForCore(ipl.disc->path),
                ipl.disc->auto_disc_change_paths);
      }

      SConfig::OnTitleDirectlyBooted(guard);
      return true;
    }

    bool operator()(const BootParameters::DFF& dff) const
    {
      NOTICE_LOG(BOOT, "Booting DFF: %s", dff.dff_path.c_str());
      return system.GetFifoPlayer().Open(dff.dff_path);
    }

  private:
    Core::System& system;
    const Core::CPUThreadGuard& guard;
    const SConfig& config;
  };

  return std::visit(BootTitle(system, guard), boot->parameters);
}

BootExecutableReader::BootExecutableReader(const std::string& file_name)
    : m_file(file_name, "rb")
{
  u64 size = 0;
  const u8* map = m_file.GetMappedPtr(&size);
  if (map)
  {
    m_bytes = map;
    m_size = size_t(size);
    return;
  }
  size = m_file.GetSize();
  if (!size || !(m_mutable = static_cast<u8*>(std::malloc(size_t(size)))))
    return;
  m_owned = true;
  if (!m_file.ReadBytes(m_mutable, size_t(size)))
    size = 0;
  m_file.Close();
  m_bytes = m_mutable;
  m_size = size_t(size);
}

BootExecutableReader::BootExecutableReader(u8* data, size_t size, bool take_ownership)
    : m_bytes(data), m_size(data ? size : 0), m_mutable(data), m_owned(take_ownership)
{
}

BootExecutableReader::~BootExecutableReader()
{
  if (m_owned)
    std::free(m_mutable);
}

u8* BootExecutableReader::MutableBytes()
{
  if (!m_mutable && m_size)
  {
    m_mutable = static_cast<u8*>(std::malloc(m_size));
    if (!m_mutable)
    {
      m_size = 0;
      return nullptr;
    }
    std::memcpy(m_mutable, m_bytes, m_size);
    m_owned = true;
    m_bytes = m_mutable;
    m_file.Close();
  }
  return m_mutable;
}

void StateFlags::UpdateChecksum()
{
  constexpr size_t length_in_bytes = sizeof(StateFlags) - 4;
  constexpr size_t num_elements = length_in_bytes / sizeof(u32);
  std::array<u32, num_elements> flag_data;
  std::memcpy(flag_data.data(), &flags, length_in_bytes);
  checksum = std::accumulate(flag_data.cbegin(), flag_data.cend(), 0U);
}

void UpdateStateFlags(const std::function<void(StateFlags*)>& update_function)
{
  CreateSystemMenuTitleDirs();
  const std::string file_path = Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_STATE;
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();
  constexpr IOS::HLE::FS::Mode rw_mode = IOS::HLE::FS::Mode::ReadWrite;
  const auto file = fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, file_path,
                                          {rw_mode, rw_mode, rw_mode});
  if (!file)
    return;

  StateFlags state{};
  if (file->GetStatus()->size == sizeof(StateFlags))
    (void)file->Read(&state, 1);

  update_function(&state);
  state.UpdateChecksum();

  (void)file->Seek(0, IOS::HLE::FS::SeekMode::Set);
  (void)file->Write(&state, 1);
}

void CreateSystemMenuTitleDirs()
{
  const auto& es = Core::System::GetInstance().GetIOS()->GetESCore();
  es.CreateTitleDirectories(Titles::SYSTEM_MENU, IOS::SYSMENU_GID);
}
