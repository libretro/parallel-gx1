// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiRoot.h"

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
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace Core
{
namespace FS = IOS::HLE::FS;

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

void InitializeWiiRoot()
{
  ASSERT(!s_wii_root_initialized);
  File::SetUserPath(D_SESSION_WIIROOT_IDX, File::GetUserPath(D_WIIROOT_IDX));
  s_wii_root_initialized = true;
}

void ShutdownWiiRoot()
{
  s_wii_root_initialized = false;
}

bool WiiRootIsInitialized()
{
  return s_wii_root_initialized;
}

void BackupWiiSettings()
{
  // Back up files which Dolphin can modify at boot, so that we can preserve the original contents.
  // For SYSCONF, the backup is only needed in case Dolphin crashes or otherwise exists unexpectedly
  // during emulation, since the config system will restore the SYSCONF settings at emulation end.
  // For setting.txt, there is no other code that restores the original values for us.

  char settings_file_path[Common::NAND_PATH_SIZE];
  Common::GetTitleDataFilePath(Titles::SYSTEM_MENU, WII_SETTING, settings_file_path,
                               sizeof(settings_file_path));
  BackupFile(settings_file_path);
  BackupFile("/shared2/sys/SYSCONF");
}

void RestoreWiiSettings(RestoreReason reason)
{
  char settings_file_path[Common::NAND_PATH_SIZE];
  Common::GetTitleDataFilePath(Titles::SYSTEM_MENU, WII_SETTING, settings_file_path,
                               sizeof(settings_file_path));
  RestoreFile(settings_file_path);

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
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path.c_str(), 0, public_modes);
      if (!CopySysmenuFilesToFS(fs, host_path, nand_path))
        return false;
    }
    else
    {
      // Do not overwrite any existing files.
      if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path.c_str()).has_value())
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

      const auto nand_file = fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID,
                                                   nand_path.c_str(), public_modes);
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
  if (!CopySysmenuFilesToFS(fs, File::GetSysDirectory() + WII_USER_DIR, ""))
    WARN_LOG(CORE, "Failed to copy initial System Menu files to the NAND");
}

}  // namespace Core
