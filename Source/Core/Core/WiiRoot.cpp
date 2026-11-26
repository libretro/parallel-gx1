// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiRoot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <retro_dirent.h>
#include <retro_miscellaneous.h>

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

// a + b on the heap, freed with free(); nullptr when out of memory
static char* Concat(const char* a, size_t a_length, const char* b)
{
  const size_t b_size = std::strlen(b) + 1;
  char* const result = static_cast<char*>(std::malloc(a_length + b_size));
  if (result)
  {
    std::memcpy(result, a, a_length);
    std::memcpy(result + a_length, b, b_size);
  }
  return result;
}

static char* UserPath(unsigned int dir_index, const char* name)
{
  const std::string& dir = File::GetUserPath(dir_index);
  return Concat(dir.c_str(), dir.size(), name);
}

// The file name in a NAND path, after its last '/'
static const char* NandFileName(const char* path_in_nand)
{
  const char* const slash = std::strrchr(path_in_nand, '/');
  return slash ? slash + 1 : path_in_nand;
}

static bool CopyBackupFile(const char* path_from, const char* path_to)
{
  if (!File::Exists(path_from))
    return false;

  File::CreateFullPath(path_to);

  return File::CopyRegularFile(path_from, path_to);
}

static void DeleteBackupFile(const char* file_name)
{
  char* const path = UserPath(D_BACKUP_IDX, file_name);
  if (path)
    File::Delete(path);
  std::free(path);
}

static void BackupFile(const char* path_in_nand)
{
  char* const original_path = UserPath(D_WIIROOT_IDX, path_in_nand);
  char* const backup_path = UserPath(D_BACKUP_IDX, NandFileName(path_in_nand));

  if (original_path && backup_path)
    CopyBackupFile(original_path, backup_path);
  std::free(backup_path);
  std::free(original_path);
}

static void RestoreFile(const char* path_in_nand)
{
  const char* const file_name = NandFileName(path_in_nand);
  char* const original_path = UserPath(D_WIIROOT_IDX, path_in_nand);
  char* const backup_path = UserPath(D_BACKUP_IDX, file_name);

  if (original_path && backup_path && CopyBackupFile(backup_path, original_path))
    DeleteBackupFile(file_name);
  std::free(backup_path);
  std::free(original_path);
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

/// Copy the directory whose host path is the host_length chars in host_path to the NAND path
/// that is the nand_length chars in nand_path; both are buffers of PATH_MAX_LENGTH that the
/// entries' paths are built in, and an entry whose path does not fit is skipped.
///
/// Both paths should not have trailing slashes. To specify the NAND root, use "".
static bool CopySysmenuFilesToFS(FS::FileSystem* fs, char* host_path, size_t host_length,
                                 char* nand_path, size_t nand_length)
{
  constexpr FS::Modes public_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  bool ok = true;
  host_path[host_length] = '\0';
  struct RDIR* const dir = retro_opendir_include_hidden(host_path, true);
  if (!dir)
    return true;

  while (ok && retro_readdir(dir))
  {
    const char* const name = retro_dirent_get_name(dir);
    if (!name || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
      continue;
    const size_t name_length = std::strlen(name);
    if (host_length + 1 + name_length >= PATH_MAX_LENGTH ||
        nand_length + 1 + name_length >= PATH_MAX_LENGTH)
    {
      continue;
    }
    host_path[host_length] = '/';
    std::memcpy(host_path + host_length + 1, name, name_length + 1);
    nand_path[nand_length] = '/';
    std::memcpy(nand_path + nand_length + 1, name, name_length + 1);

    if (retro_dirent_is_dir(dir, nullptr))
    {
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, 0, public_modes);
      ok = CopySysmenuFilesToFS(fs, host_path, host_length + 1 + name_length, nand_path,
                                nand_length + 1 + name_length);
      continue;
    }

    // Do not overwrite any existing files.
    if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path).has_value())
      continue;

    // written into the NAND from the mapping where the host file has one
    File::IOFile host_file{host_path, "rb"};
    u64 size = 0;
    const u8* data = host_file.GetMappedPtr(&size);
    u8* file_data = nullptr;
    if (!data)
    {
      size = host_file.GetSize();
      file_data = static_cast<u8*>(std::malloc(size ? size : 1));
      if (!file_data || !host_file.ReadBytes(file_data, size))
      {
        std::free(file_data);
        ok = false;
        continue;
      }
      data = file_data;
    }

    const auto nand_file =
        fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, public_modes);
    if (!nand_file || !nand_file->Write(data, size))
      ok = false;
    std::free(file_data);
  }
  retro_closedir(dir);
  return ok;
}

void InitializeWiiFileSystemContents(const BootSessionData& boot_session_data)
{
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();

  // Some games (such as Mario Kart Wii) assume that NWC24 files will always be present
  // even upon the first launch as they are normally created by the system menu.
  // Because we do not require the system menu to be run, WiiConnect24 files must be copied
  // to the NAND manually.
  const std::string& sys_directory = File::GetSysDirectory();
  char* const host_path = static_cast<char*>(std::malloc(PATH_MAX_LENGTH));
  char* const nand_path = static_cast<char*>(std::malloc(PATH_MAX_LENGTH));
  const size_t host_length = sys_directory.size() + sizeof(WII_USER_DIR) - 1;
  bool copied = false;
  if (host_path && nand_path && host_length < PATH_MAX_LENGTH)
  {
    std::memcpy(host_path, sys_directory.c_str(), sys_directory.size());
    std::memcpy(host_path + sys_directory.size(), WII_USER_DIR, sizeof(WII_USER_DIR));
    nand_path[0] = '\0';
    copied = CopySysmenuFilesToFS(fs, host_path, host_length, nand_path, 0);
  }
  std::free(nand_path);
  std::free(host_path);
  if (!copied)
    WARN_LOG(CORE, "Failed to copy initial System Menu files to the NAND");
}

}  // namespace Core
