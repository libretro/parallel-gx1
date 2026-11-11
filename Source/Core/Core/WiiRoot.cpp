// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiRoot.h"

#include <cinttypes>
#include <optional>
#include <string>
#include <vector>

#include <cstdio>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/SysConf.h"
#include "Core/System.h"

namespace Core
{
namespace FS = IOS::HLE::FS;

static std::string s_temp_wii_root;
static bool s_wii_root_initialized = false;

static bool CopyBackupFile(const std::string& path_from, const std::string& path_to)
{
  if (!File::Exists(path_from))
    return false;

  File::CreateFullPath(path_to);

  return File::CopyRegularFile(path_from, path_to);
}

static void DeleteBackupFile(const std::string& file_name)
{
  File::Delete(File::GetUserPath(D_BACKUP_IDX) + file_name);
}

static void BackupFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + file_name;

  CopyBackupFile(original_path, backup_path);
}

static void RestoreFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + file_name;

  if (CopyBackupFile(backup_path, original_path))
    DeleteBackupFile(file_name);
}

static bool CopyNandFile(FS::FileSystem* source_fs, const std::string& source_file,
                         FS::FileSystem* dest_fs, const std::string& dest_file)
{
  auto source_handle =
      source_fs->OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, source_file, IOS::HLE::FS::Mode::Read);
  // If the source file doesn't exist, there is nothing more to do.
  // This function must not create an empty file on the destination filesystem.
  if (!source_handle)
    return true;

  dest_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, dest_file, 0,
                          {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite});

  auto dest_handle =
      dest_fs->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, source_file,
                                 {IOS::HLE::FS::Mode::ReadWrite, IOS::HLE::FS::Mode::ReadWrite,
                                  IOS::HLE::FS::Mode::ReadWrite});
  if (!dest_handle)
    return false;

  std::vector<u8> buffer(source_handle->GetStatus()->size);
  if (!source_handle->Read(buffer.data(), buffer.size()))
    return false;
  if (!dest_handle->Write(buffer.data(), buffer.size()))
    return false;

  return true;
}

static void MoveToBackupIfExists(const std::string& path)
{
  if (File::Exists(path))
  {
    const std::string backup_path = path.substr(0, path.size() - 1) + ".backup";
    WARN_LOG(IOS_FS, "Temporary directory at %s exists, moving to backup...", path.c_str());

    // If backup exists, delete it as we don't want a mess
    if (File::Exists(backup_path))
    {
      WARN_LOG(IOS_FS, "Temporary backup directory at %s exists, deleting...", backup_path.c_str());
      File::DeleteDirRecursively(backup_path);
    }

    File::MoveWithOverwrite(path, backup_path);
  }
}

void InitializeWiiRoot(bool use_temporary)
{
  ASSERT(!s_wii_root_initialized);

  if (use_temporary)
  {
    s_temp_wii_root = File::GetUserPath(D_USER_IDX) + "WiiSession" DIR_SEP;
    WARN_LOG(IOS_FS, "Using temporary directory %s for minimal Wii FS", s_temp_wii_root.c_str());

    // If directory exists, make a backup
    MoveToBackupIfExists(s_temp_wii_root);

    File::SetUserPath(D_SESSION_WIIROOT_IDX, s_temp_wii_root);
  }
  else
  {
    File::SetUserPath(D_SESSION_WIIROOT_IDX, File::GetUserPath(D_WIIROOT_IDX));
  }

  s_wii_root_initialized = true;
}

void ShutdownWiiRoot()
{
  if (WiiRootIsTemporary())
  {
    File::DeleteDirRecursively(s_temp_wii_root);
    s_temp_wii_root.clear();
  }

  s_wii_root_initialized = false;
}

bool WiiRootIsInitialized()
{
  return s_wii_root_initialized;
}

bool WiiRootIsTemporary()
{
  return !s_temp_wii_root.empty();
}

void BackupWiiSettings()
{
  // Back up files which Dolphin can modify at boot, so that we can preserve the original contents.
  // For SYSCONF, the backup is only needed in case Dolphin crashes or otherwise exists unexpectedly
  // during emulation, since the config system will restore the SYSCONF settings at emulation end.
  // For setting.txt, there is no other code that restores the original values for us.

  BackupFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);
  BackupFile("/shared2/sys/SYSCONF");
}

void RestoreWiiSettings(RestoreReason reason)
{
  RestoreFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);

  // We must not restore the SYSCONF backup when ending emulation cleanly, since the user may have
  // edited the SYSCONF file in the NAND using the emulated software (e.g. the Wii Menu settings).
  if (reason == RestoreReason::CrashRecovery)
    RestoreFile("/shared2/sys/SYSCONF");
  else
    DeleteBackupFile("SYSCONF");
}

/// Copy a directory from host_source_path (on the host FS) to nand_target_path on the NAND.
///
/// Both paths should not have trailing slashes. To specify the NAND root, use "".
static bool CopySysmenuFilesToFS(FS::FileSystem* fs, const std::string& host_source_path,
                                 const std::string& nand_target_path)
{
  const auto entries = File::ScanDirectoryTree(host_source_path, false);
  for (const File::FSTEntry& entry : entries.children)
  {
    const std::string host_path = host_source_path + '/' + entry.virtualName;
    const std::string nand_path = nand_target_path + '/' + entry.virtualName;
    constexpr FS::Modes public_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};

    if (entry.isDirectory)
    {
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, 0, public_modes);
      if (!CopySysmenuFilesToFS(fs, host_path, nand_path))
        return false;
    }
    else
    {
      // Do not overwrite any existing files.
      if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path).has_value())
        continue;

      // written into the NAND from the mapping where the host file has one
      File::IOFile host_file{host_path, "rb"};
      u64 size = 0;
      const u8* data = host_file.GetMappedPtr(&size);
      std::vector<u8> file_data;
      if (!data)
      {
        file_data.resize(host_file.GetSize());
        if (!host_file.ReadBytes(file_data.data(), file_data.size()))
          return false;
        data = file_data.data();
        size = file_data.size();
      }

      const auto nand_file =
          fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, public_modes);
      if (!nand_file || !nand_file->Write(data, size))
        return false;
    }
  }
  return true;
}

void InitializeWiiFileSystemContents(const BootSessionData& boot_session_data)
{
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();

  // Some games (such as Mario Kart Wii) assume that NWC24 files will always be present
  // even upon the first launch as they are normally created by the system menu.
  // Because we do not require the system menu to be run, WiiConnect24 files must be copied
  // to the NAND manually.
  if (!CopySysmenuFilesToFS(fs.get(), File::GetSysDirectory() + WII_USER_DIR, ""))
    WARN_LOG(CORE, "Failed to copy initial System Menu files to the NAND");

  if (WiiRootIsTemporary())
  {
    // Generate a SYSCONF with default settings for the temporary Wii NAND.
    SysConf sysconf{fs};
    sysconf.Save();
  }
}

void CleanUpWiiFileSystemContents(const BootSessionData& boot_session_data)
{
  // In TAS mode, copy back always.
  const bool cleanup_required = WiiRootIsTemporary();

  INFO_LOG(CORE, "Wii FS Cleanup: cleanup_required = %s", cleanup_required ? "true" : "false");

  if (!cleanup_required)
    return;

  INFO_LOG(CORE, "Wii FS Cleanup: Copying from temporary FS to configured_fs.");

  IOS::HLE::EmulationKernel* ios = Core::System::GetInstance().GetIOS();

  const auto configured_fs = FS::MakeFileSystem(FS::Location::Configured);

  // Copy back Mii data
  if (!CopyNandFile(ios->GetFS().get(), Common::GetMiiDatabasePath(), configured_fs.get(),
                    Common::GetMiiDatabasePath()))
  {
    WARN_LOG(CORE, "Failed to copy Mii database to the NAND");
  }

  for (const u64 title_id : ios->GetESCore().GetInstalledTitles())
  {
    INFO_LOG(CORE, "Wii FS Cleanup: Copying %016" PRIx64 ".", (u64)title_id);

    const auto session_save = WiiSave::MakeNandStorage(ios->GetFS().get(), title_id);

    // FS won't write the save if the directory doesn't exist
    const std::string title_path = Common::GetTitleDataPath(title_id);
    configured_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, title_path + '/', 0,
                                  {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite});

    const auto user_save = WiiSave::MakeNandStorage(configured_fs.get(), title_id);

    const std::string backup_path = StringFromFormat(
        "%s/%016" PRIx64 ".bin", File::GetUserPath(D_BACKUP_IDX).c_str(), (u64)title_id);
    const auto backup_save = WiiSave::MakeDataBinStorage(&ios->GetIOSC(), backup_path, "w+b");

    // Backup the existing save just in case it's still needed.
    WiiSave::Copy(user_save.get(), backup_save.get());
    WiiSave::Copy(session_save.get(), user_save.get());
  }
}
}  // namespace Core
