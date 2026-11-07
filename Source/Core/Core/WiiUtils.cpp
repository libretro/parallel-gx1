// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiUtils.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"
#include "Core/IOS/Uids.h"
#include "Core/SysConf.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeWad.h"

namespace WiiUtils
{
static bool CreateShopLogs(IOS::HLE::FS::FileSystem* fs)
{
  static const char* const paths[] = {"/shared2/ec/shopsetu.log", "/shared2/succession/shop.log"};
  static const u8 dummy_data[32] = {};
  constexpr IOS::HLE::FS::Mode rw_mode = IOS::HLE::FS::Mode::ReadWrite;
  size_t i;
  for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
  {
    if (fs->CreateFullPath(IOS::SYSMENU_UID, IOS::SYSMENU_GID, paths[i], 0,
                           {rw_mode, rw_mode, rw_mode}) != IOS::HLE::FS::ResultCode::Success)
    {
      return false;
    }

    if (fs->OpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, paths[i], rw_mode))
      continue;

    const auto new_handle = fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, paths[i],
                                                  {rw_mode, rw_mode, rw_mode});
    if (!new_handle || !new_handle->Write(dummy_data, sizeof(dummy_data)))
      return false;
  }
  return true;
}

static bool ImportWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad,
                      IOS::HLE::ESCore::VerifySignature verify_signature)
{
  if (!wad.GetTicket().IsValid() || !wad.GetTMD().IsValid())
  {
    PanicAlertFmtT("WAD installation failed: The selected file is not a valid WAD.");
    return false;
  }

  const IOS::ES::TMDReader& tmd = wad.GetTMD();
  auto& es = ios.GetESCore();
  const auto fs = ios.GetFS();

  IOS::HLE::ESCore::Context context;
  IOS::HLE::ReturnCode ret;

  // Ensure the common key index is correct, as it's checked by IOS.
  IOS::ES::TicketReader ticket = wad.GetTicketWithFixedCommonKey();
  const std::vector<u8>& cert_chain = wad.GetCertificateChain();
  size_t ticket_size;
  const u8* const ticket_bytes = ticket.GetBytes(&ticket_size);
  size_t tmd_size;
  const u8* const tmd_bytes = tmd.GetBytes(&tmd_size);

  if ((ret = es.ImportTicket(ticket_bytes, ticket_size, cert_chain.data(), cert_chain.size(),
                             IOS::HLE::ESCore::TicketImportType::Unpersonalised,
                             verify_signature)) < 0 ||
      (ret = es.ImportTitleInit(context, tmd_bytes, tmd_size, cert_chain.data(),
                                cert_chain.size(), verify_signature)) < 0)
  {
    if (ret != IOS::HLE::IOSC_FAIL_CHECKVALUE)
    {
      PanicAlertFmtT("WAD installation failed: Could not initialise title import (error {0}).",
                     std::to_underlying(ret));
    }
    return false;
  }

  bool contents_imported = true;
  const u64 title_id = tmd.GetTitleId();
  for (const IOS::ES::Content& content : tmd.GetContents())
  {
    const std::vector<u8> data = wad.GetContent(content.index);

    if (es.ImportContentBegin(context, title_id, content.id) < 0 ||
        es.ImportContentData(context, 0, data.data(), static_cast<u32>(data.size())) < 0 ||
        es.ImportContentEnd(context, 0) < 0)
    {
      PanicAlertFmtT("WAD installation failed: Could not import content {0:08x}.", content.id);
      contents_imported = false;
      break;
    }
  }

  if ((contents_imported && es.ImportTitleDone(context) < 0) ||
      (!contents_imported && es.ImportTitleCancel(context) < 0))
  {
    PanicAlertFmtT("WAD installation failed: Could not finalise title import.");
    return false;
  }

  // Under normal conditions, these two log files are created by the Wii Shop channel at some point
  // during the process of downloading a game, and some games (eg. Mega Man 9) refuse to load DLC if
  // they are not present. So ensure they exist and create them if they don't.
  if (!CreateShopLogs(fs.get()))
  {
    PanicAlertFmtT("WAD installation failed: Could not create Wii Shop log files.");
    return false;
  }

  return true;
}

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType install_type)
{
  if (!wad.GetTMD().IsValid())
    return false;

  SysConf sysconf{ios.GetFS()};
  SysConf::Entry* tid_entry = sysconf.GetOrAddEntry("IPL.TID", SysConf::Entry::Type::LongLong);
  const u64 previous_temporary_title_id = Common::swap64(tid_entry->GetData<u64>(0));
  const u64 title_id = wad.GetTMD().GetTitleId();

  // Skip the install if the WAD is already installed.
  const auto installed_contents = ios.GetESCore().GetStoredContentsFromTMD(
      wad.GetTMD(), IOS::HLE::ESCore::CheckContentHashes::Yes);
  if (wad.GetTMD().GetContents() == installed_contents)
  {
    // Clear the "temporary title ID" flag in case the user tries to permanently install a title
    // that has already been imported as a temporary title.
    if (previous_temporary_title_id == title_id && install_type == InstallType::Permanent)
      tid_entry->SetData<u64>(0);
    return true;
  }

  // If a different version is currently installed, warn the user to make sure
  // they don't overwrite the current version by mistake.
  const IOS::ES::TMDReader installed_tmd = ios.GetESCore().FindInstalledTMD(title_id);
  const bool has_another_version =
      installed_tmd.IsValid() && installed_tmd.GetTitleVersion() != wad.GetTMD().GetTitleVersion();
  if (has_another_version &&
      !AskYesNoFmtT("A different version of this title is already installed on the NAND.\n\n"
                    "Installed version: {0}\nWAD version: {1}\n\n"
                    "Installing this WAD will replace it irreversibly. Continue?",
                    installed_tmd.GetTitleVersion(), wad.GetTMD().GetTitleVersion()))
  {
    return false;
  }

  // Delete a previous temporary title, if it exists.
  if (previous_temporary_title_id)
    ios.GetESCore().DeleteTitleContent(previous_temporary_title_id);

  // A lot of people use fakesigned WADs, so disable signature checking when installing a WAD.
  if (!ImportWAD(ios, wad, IOS::HLE::ESCore::VerifySignature::No))
    return false;

  // Keep track of the title ID so this title can be removed to make room for any future install.
  // We use the same mechanism as the System Menu for temporary SD card title data.
  if (!has_another_version && install_type == InstallType::Temporary)
    tid_entry->SetData<u64>(Common::swap64(title_id));
  else
    tid_entry->SetData<u64>(0);

  return true;
}

static bool IsTMDImported(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  const auto entries = fs.ReadDirectory(0, 0, Common::GetTitleContentPath(title_id));
  if (!entries)
    return false;
  for (const std::string& file : *entries)
  {
    if (file == "title.tmd")
      return true;
  }
  return false;
}

static IOS::ES::TMDReader FindBackupTMD(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  auto file = fs.OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                          "/title/00000001/00000002/data/tmds.sys", IOS::HLE::FS::Mode::Read);
  if (!file)
    return {};

  // structure of this file is as follows:
  // - 32 bytes descriptor of a TMD, which contains a title ID and a length
  // - the TMD, with padding aligning to 32 bytes
  // - repeat for as many TMDs as stored
  while (true)
  {
    std::array<u8, 32> descriptor;
    if (!file->Read(descriptor.data(), descriptor.size()))
      return {};

    const u64 tid = Common::swap64(descriptor.data());
    const u32 tmd_length = Common::swap32(descriptor.data() + 8);
    if (tid == title_id)
    {
      // found the right TMD
      // read straight into the reader
      IOS::ES::TMDReader tmd;
      u8* const bytes = tmd.ResizeBytes(tmd_length);
      if (tmd_length && (!bytes || !file->Read(bytes, tmd_length)))
        return {};
      return tmd;
    }

    // not the right TMD, skip this one and go to the next
    if (!file->Seek(Common::AlignUp(tmd_length, 32), IOS::HLE::FS::SeekMode::Current))
      return {};
  }
}

bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id)
{
  if (IsTMDImported(fs, title_id))
    return true;

  auto tmd = FindBackupTMD(fs, title_id);
  if (!tmd.IsValid())
    return false;

  IOS::HLE::ESCore::Context context;
  context.uid = IOS::SYSMENU_UID;
  context.gid = IOS::SYSMENU_GID;
  size_t tmd_size;
  const u8* const tmd_bytes = tmd.GetBytes(&tmd_size);
  const auto import_result = es.ImportTmd(context, tmd_bytes, tmd_size, Titles::SYSTEM_MENU,
                                          IOS::ES::TITLE_TYPE_DEFAULT);
  if (import_result != IOS::HLE::IPC_SUCCESS)
    return false;

  return es.ImportTitleDone(context) == IOS::HLE::IPC_SUCCESS;
}

static std::shared_ptr<IOS::HLE::Device> GetBluetoothDevice()
{
  auto* ios = Core::System::GetInstance().GetIOS();
  return ios ? ios->GetDeviceByName("/dev/usb/oh1/57e/305") : nullptr;
}

std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice()
{
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothEmuDevice>(GetBluetoothDevice());
}

std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice()
{
  if (!Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothRealDevice>(GetBluetoothDevice());
}
}  // namespace WiiUtils
