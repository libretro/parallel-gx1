// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FatFsUtil.h"

#include <cinttypes>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Does not compile if diskio.h is included first.
// clang-format off
#include "ff.h"
#include "diskio.h"
// clang-format on

#include <retro_atomic.h>
#include "Common/Align.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/TimeUtil.h"

#include "Core/Config/MainSettings.h"

enum : u32
{
  SECTOR_SIZE = 512,
  MAX_CLUSTER_SIZE = 64 * SECTOR_SIZE,
};

// FatFs keeps global state, so one user at a time. Users are rare and never overlap in
// practice; one that finds FatFs taken gives up with an error instead of waiting.
static retro_atomic_int_t s_fatfs_in_use;

namespace
{
class FatFsUse
{
public:
  FatFsUse() : m_owned{retro_atomic_cas_int(&s_fatfs_in_use, 0, 1) != 0}
  {
    if (!m_owned)
      ERROR_LOG(COMMON, "FatFs is busy on another thread");
  }
  ~FatFsUse()
  {
    if (m_owned)
      retro_atomic_store_release_int(&s_fatfs_in_use, 0);
  }
  FatFsUse(const FatFsUse&) = delete;
  FatFsUse& operator=(const FatFsUse&) = delete;
  explicit operator bool() const { return m_owned; }

private:
  bool m_owned;
};
}  // namespace
static Common::FatFsCallbacks* s_callbacks;

namespace
{
int SDCardDiskRead(File::IOFile* image, u8 pdrv, u8* buff, u32 sector, unsigned int count)
{
  const u64 offset = static_cast<u64>(sector) * SECTOR_SIZE;
  if (!image->Seek(offset, File::SeekOrigin::Begin))
  {
    ERROR_LOG(COMMON, "SD image seek failed (offset=%" PRIu64 ")", (u64)offset);
    return RES_ERROR;
  }

  const size_t size = static_cast<size_t>(count) * SECTOR_SIZE;
  if (!image->ReadBytes(buff, size))
  {
    ERROR_LOG(COMMON, "SD image read failed (offset=%" PRIu64 ", size=%" PRIu64 ")", (u64)offset,
              (u64)size);
    return RES_ERROR;
  }

  return RES_OK;
}

int SDCardDiskWrite(File::IOFile* image, u8 pdrv, const u8* buff, u32 sector, unsigned int count)
{
  const u64 offset = static_cast<u64>(sector) * SECTOR_SIZE;
  if (!image->Seek(offset, File::SeekOrigin::Begin))
  {
    ERROR_LOG(COMMON, "SD image seek failed (offset=%" PRIu64 ")", (u64)offset);
    return RES_ERROR;
  }

  const size_t size = static_cast<size_t>(count) * SECTOR_SIZE;
  if (!image->WriteBytes(buff, size))
  {
    ERROR_LOG(COMMON, "SD image write failed (offset=%" PRIu64 ", size=%" PRIu64 ")", (u64)offset,
              (u64)size);
    return RES_ERROR;
  }

  return RES_OK;
}

int SDCardDiskIOCtl(File::IOFile* image, u8 pdrv, u8 cmd, void* buff)
{
  switch (cmd)
  {
  case CTRL_SYNC:
    return RES_OK;
  case GET_SECTOR_COUNT:
    *static_cast<LBA_t*>(buff) = image->GetSize() / SECTOR_SIZE;
    return RES_OK;
  default:
    WARN_LOG(COMMON, "Unexpected SD image ioctl %u", (unsigned)cmd);
    return RES_OK;
  }
}

u32 GetSystemTimeFAT()
{
  const std::time_t time = std::time(nullptr);
  std::tm tm = *Common::LocalTime(time);

  DWORD fattime = 0;
  fattime |= (tm.tm_year - 80) << 25;
  fattime |= (tm.tm_mon + 1) << 21;
  fattime |= tm.tm_mday << 16;
  fattime |= tm.tm_hour << 11;
  fattime |= tm.tm_min << 5;
  fattime |= (tm.tm_sec < 59 ? tm.tm_sec : 59) >> 1;
  return fattime;
}
}  // namespace

namespace Common
{
FatFsCallbacks::FatFsCallbacks() = default;
FatFsCallbacks::~FatFsCallbacks() = default;

u8 FatFsCallbacks::DiskInitialize(u8 pdrv)
{
  return 0;
}

u8 FatFsCallbacks::DiskStatus(u8 pdrv)
{
  return 0;
}

u32 FatFsCallbacks::GetCurrentTimeFAT()
{
  return GetSystemTimeFAT();
}
}  // namespace Common

namespace
{
class SDCardFatFsCallbacks : public Common::FatFsCallbacks
{
public:
  int DiskRead(u8 pdrv, u8* buff, u32 sector, unsigned int count) override
  {
    return SDCardDiskRead(m_image, pdrv, buff, sector, count);
  }

  int DiskWrite(u8 pdrv, const u8* buff, u32 sector, unsigned int count) override
  {
    return SDCardDiskWrite(m_image, pdrv, buff, sector, count);
  }

  int DiskIOCtl(u8 pdrv, u8 cmd, void* buff) override
  {
    return SDCardDiskIOCtl(m_image, pdrv, cmd, buff);
  }

  u32 GetCurrentTimeFAT() override
  {
    if (m_deterministic)
      return 0;

    return GetSystemTimeFAT();
  }

  File::IOFile* m_image = nullptr;
  bool m_deterministic = false;
};
}  // namespace

extern "C" DSTATUS disk_status(BYTE pdrv)
{
  return static_cast<DSTATUS>(s_callbacks->DiskStatus(pdrv));
}

extern "C" DSTATUS disk_initialize(BYTE pdrv)
{
  return static_cast<DSTATUS>(s_callbacks->DiskInitialize(pdrv));
}

extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
  return static_cast<DRESULT>(s_callbacks->DiskRead(pdrv, buff, sector, count));
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
  return static_cast<DRESULT>(s_callbacks->DiskWrite(pdrv, buff, sector, count));
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
  return static_cast<DRESULT>(s_callbacks->DiskIOCtl(pdrv, cmd, buff));
}

extern "C" DWORD get_fattime(void)
{
  return static_cast<DWORD>(s_callbacks->GetCurrentTimeFAT());
}

#if FF_USE_LFN == 3  // match ff.h; currently unused by Dolphin
extern "C" void* ff_memalloc(UINT msize)
{
  return std::malloc(msize);
}

extern "C" void ff_memfree(void* mblock)
{
  return std::free(mblock);
}
#endif

static const char* FatFsErrorToString(FRESULT error_code)
{
  // These are taken from the comment next to each value in ff.h
  switch (error_code)
  {
  case FR_OK:
    return "Succeeded";
  case FR_DISK_ERR:
    return "A hard error occurred in the low level disk I/O layer";
  case FR_INT_ERR:
    return "Assertion failed";
  case FR_NOT_READY:
    return "The physical drive cannot work";
  case FR_NO_FILE:
    return "Could not find the file";
  case FR_NO_PATH:
    return "Could not find the path";
  case FR_INVALID_NAME:
    return "The path name format is invalid";
  case FR_DENIED:
    return "Access denied due to prohibited access or directory full";
  case FR_EXIST:
    return "Access denied due to prohibited access";
  case FR_INVALID_OBJECT:
    return "The file/directory object is invalid";
  case FR_WRITE_PROTECTED:
    return "The physical drive is write protected";
  case FR_INVALID_DRIVE:
    return "The logical drive number is invalid";
  case FR_NOT_ENABLED:
    return "The volume has no work area";
  case FR_NO_FILESYSTEM:
    return "There is no valid FAT volume";
  case FR_MKFS_ABORTED:
    return "The f_mkfs() aborted due to any problem";
  case FR_TIMEOUT:
    return "Could not get a grant to access the volume within defined period";
  case FR_LOCKED:
    return "The operation is rejected according to the file sharing policy";
  case FR_NOT_ENOUGH_CORE:
    return "LFN working buffer could not be allocated";
  case FR_TOO_MANY_OPEN_FILES:
    return "Number of open files > FF_FS_LOCK";
  case FR_INVALID_PARAMETER:
    return "Given parameter is invalid";
  default:
    return "Unknown error";
  }
}

namespace Common
{
static constexpr u64 MebibytesToBytes(u64 mebibytes)
{
  return mebibytes * 1024 * 1024;
}

static constexpr u64 GibibytesToBytes(u64 gibibytes)
{
  return gibibytes * 1024 * 1024 * 1024;
}

static bool CheckIfFATCompatible(const File::FSTEntry& entry)
{
  if (!entry.isDirectory)
    return true;

  if (entry.children.size() > 65536)
  {
    ERROR_LOG(COMMON, "Directory %s has too many entries (%" PRIu64 ")", entry.physicalName.c_str(),
              (u64)entry.children.size());
    return false;
  }

  for (const File::FSTEntry& child : entry.children)
  {
    const size_t size = UTF8ToUTF16(child.virtualName).size();
    if (size > 255)
    {
      ERROR_LOG(COMMON, "Filename %s (in directory %s) is too long (%" PRIu64 ")",
                child.virtualName.c_str(), entry.physicalName.c_str(), (u64)size);
      return false;
    }

    if (child.size >= GibibytesToBytes(4))
    {
      ERROR_LOG(COMMON, "File %s (in directory %s) is too large (%" PRIu64 ")",
                child.virtualName.c_str(), entry.physicalName.c_str(), (u64)child.size);
      return false;
    }

    if (!CheckIfFATCompatible(child))
      return false;
  }

  return true;
}

static u64 GetSize(const File::FSTEntry& entry)
{
  if (!entry.isDirectory)
    return AlignUp(entry.size, MAX_CLUSTER_SIZE);

  u64 size = 0;
  for (const File::FSTEntry& child : entry.children)
  {
    size += 32;
    // For simplicity, assume that all names are LFN.
    const u64 num_lfn_entries = (UTF8ToUTF16(child.virtualName).size() + 13 - 1) / 13;
    size += num_lfn_entries * 32;
  }
  size = AlignUp(size, MAX_CLUSTER_SIZE);

  for (const File::FSTEntry& child : entry.children)
    size += GetSize(child);

  return size;
}

static bool Pack(const std::function<bool()>& cancelled, const File::FSTEntry& entry, bool is_root,
                 std::vector<u8>& tmp_buffer)
{
  if (cancelled())
    return false;

  if (!entry.isDirectory)
  {
    File::IOFile src(entry.physicalName, "rb");
    if (!src)
    {
      ERROR_LOG(COMMON, "Failed to open file %s", entry.physicalName.c_str());
      return false;
    }

    FIL dst{};
    const auto open_error_code =
        f_open(&dst, entry.virtualName.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (open_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to open file %s in SD image: %s", entry.physicalName.c_str(),
                FatFsErrorToString(open_error_code));
      return false;
    }

    const size_t src_size = src.GetSize();
    if (src.GetSize() != entry.size)
    {
      ERROR_LOG(COMMON,
                "File at %s does not match previously read filesize (%" PRIu64 " != %" PRIu64 ")",
                entry.physicalName.c_str(), (u64)entry.size, (u64)src_size);
      return false;
    }

    if (entry.size >= GibibytesToBytes(4))
    {
      ERROR_LOG(COMMON, "File at %s is too large to fit into FAT (%" PRIu64 " >= 4GiB)",
                entry.physicalName.c_str(), (u64)entry.size);
      return false;
    }

    u64 size = entry.size;
    while (size > 0)
    {
      if (cancelled())
        return false;

      u32 chunk_size =
          static_cast<u32>(size < tmp_buffer.size() ? size : u64(tmp_buffer.size()));
      if (!src.ReadBytes(tmp_buffer.data(), chunk_size))
      {
        ERROR_LOG(COMMON, "Failed to read data from file at %s", entry.physicalName.c_str());
        return false;
      }

      u32 written_size;
      const auto write_error_code = f_write(&dst, tmp_buffer.data(), chunk_size, &written_size);
      if (write_error_code != FR_OK)
      {
        ERROR_LOG(COMMON, "Failed to write file %s to SD image: %s", entry.physicalName.c_str(),
                  FatFsErrorToString(write_error_code));
        return false;
      }

      if (written_size != chunk_size)
      {
        ERROR_LOG(COMMON, "Failed to write bytes of file %s to SD image (%u != %u)",
                  entry.physicalName.c_str(), written_size, chunk_size);
        return false;
      }

      size -= chunk_size;
    }

    const auto close_error_code = f_close(&dst);
    if (close_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to close file %s in SD image: %s", entry.physicalName.c_str(),
                FatFsErrorToString(close_error_code));
      return false;
    }

    if (!src.Close())
    {
      ERROR_LOG(COMMON, "Failed to close file %s", entry.physicalName.c_str());
      return false;
    }

    return true;
  }

  if (!is_root)
  {
    const auto mkdir_error_code = f_mkdir(entry.virtualName.c_str());
    if (mkdir_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to make directory %s in SD image: %s", entry.physicalName.c_str(),
                FatFsErrorToString(mkdir_error_code));
      return false;
    }

    const auto chdir_error_code = f_chdir(entry.virtualName.c_str());
    if (chdir_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to entry directory %s in SD image: %s", entry.physicalName.c_str(),
                FatFsErrorToString(chdir_error_code));
      return false;
    }
  }

  for (const File::FSTEntry& child : entry.children)
  {
    if (!Pack(cancelled, child, false, tmp_buffer))
      return false;
  }

  if (!is_root)
  {
    const auto chdir_up_error_code = f_chdir("..");
    if (chdir_up_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to leave directory %s in SD image: %s", entry.physicalName.c_str(),
                FatFsErrorToString(chdir_up_error_code));
      return false;
    }
  }

  return true;
}

static void SiftDown(File::FSTEntry* entries, size_t root, size_t count)
{
  for (;;)
  {
    size_t child = root * 2 + 1;
    if (child >= count)
      return;
    if (child + 1 < count && entries[child].virtualName < entries[child + 1].virtualName)
      child++;
    if (!(entries[root].virtualName < entries[child].virtualName))
      return;
    std::swap(entries[root], entries[child]);
    root = child;
  }
}

static void SortFST(File::FSTEntry* root)
{
  // in-place heap sort by name
  File::FSTEntry* const entries = root->children.data();
  const size_t count = root->children.size();
  size_t i;
  if (count > 1)
  {
    for (i = count / 2; i > 0; i--)
      SiftDown(entries, i - 1, count);
    for (i = count - 1; i > 0; i--)
    {
      std::swap(entries[0], entries[i]);
      SiftDown(entries, 0, i);
    }
  }
  for (auto& child : root->children)
    SortFST(&child);
}

bool SyncSDFolderToSDImage(const std::function<bool()>& cancelled, bool deterministic)
{
  const std::string source_dir = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  const std::string image_path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  if (source_dir.empty() || image_path.empty())
    return false;

  INFO_LOG(COMMON, "Starting SD card conversion from folder %s to file %s", source_dir.c_str(),
           image_path.c_str());

  if (!File::IsDirectory(source_dir))
  {
    ERROR_LOG(COMMON, "%s is not a directory, not converting", source_dir.c_str());
    return false;
  }

  File::FSTEntry root = File::ScanDirectoryTree(source_dir, true);
  if (deterministic)
    SortFST(&root);
  if (!CheckIfFATCompatible(root))
    return false;

  u64 size = Config::Get(Config::MAIN_WII_SD_CARD_FILESIZE);
  if (size == 0)
  {
    size = GetSize(root);
    // Allocate a reasonable amount of free space
    const u64 extra = size / 2;
    size += extra < MebibytesToBytes(512) ? MebibytesToBytes(512) :
            extra > GibibytesToBytes(8)  ? GibibytesToBytes(8) :
                                           extra;
  }
  size = AlignUp(size, MAX_CLUSTER_SIZE);

  const FatFsUse fatfs_use;
  if (!fatfs_use)
    return false;
  SDCardFatFsCallbacks callbacks;
  s_callbacks = &callbacks;
  Common::ScopeGuard callbacks_guard{[] { s_callbacks = nullptr; }};

  File::IOFile image;
  callbacks.m_image = &image;
  callbacks.m_deterministic = deterministic;

  const std::string temp_image_path = File::GetTempFilenameForAtomicWrite(image_path);
  if (!image.Open(temp_image_path, "w+b"))
  {
    ERROR_LOG(COMMON, "Failed to create or overwrite SD image at %s", image_path.c_str());
    return false;
  }

  // delete temp file in failure case
  Common::ScopeGuard image_delete_guard{[&] {
    image.Close();
    File::Delete(temp_image_path);
  }};

  if (!image.Resize(size))
  {
    ERROR_LOG(COMMON, "Failed to allocate %" PRIu64 " bytes for SD image at %s", (u64)size,
              image_path.c_str());
    return false;
  }

  MKFS_PARM options = {};
  options.fmt = FM_FAT32 | FM_SFD;
  options.n_fat = 0;    // Number of FATs: automatic
  options.align = 1;    // Alignment of the data region (in sectors)
  options.n_root = 0;   // Number of root directory entries: automatic (and unused for FAT32)
  options.au_size = 0;  // Cluster size: automatic

  std::vector<u8> tmp_buffer(MAX_CLUSTER_SIZE);
  const auto mkfs_error_code =
      f_mkfs("", &options, tmp_buffer.data(), static_cast<UINT>(tmp_buffer.size()));
  if (mkfs_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to initialize SD image filesystem: %s",
              FatFsErrorToString(mkfs_error_code));
    return false;
  }

  FATFS fs{};
  const auto mount_error_code = f_mount(&fs, "", 0);
  if (mount_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to mount SD image filesystem: %s",
              FatFsErrorToString(mount_error_code));
    return false;
  }
  Common::ScopeGuard unmount_guard{[] { f_unmount(""); }};

  if (!Pack(cancelled, root, true, tmp_buffer))
  {
    ERROR_LOG(COMMON, "Failed to pack folder %s to SD image at %s", source_dir.c_str(),
              temp_image_path.c_str());
    return false;
  }

  unmount_guard.Exit();  // unmount before closing the image

  if (!image.Close())
  {
    ERROR_LOG(COMMON, "Failed to close SD image at %s", temp_image_path.c_str());
    return false;
  }

  if (!File::Rename(temp_image_path, image_path))
  {
    ERROR_LOG(COMMON, "Failed to rename SD image from %s to %s", temp_image_path.c_str(),
              image_path.c_str());
    return false;
  }

  image_delete_guard.Dismiss();  // no need to delete the temp file anymore after the rename

  INFO_LOG(COMMON, "Successfully packed folder %s to SD image at %s", source_dir.c_str(),
           image_path.c_str());
  return true;
}

static bool Unpack(const std::function<bool()>& cancelled, const std::string& path,
                   bool is_directory, const char* name, std::vector<u8>& tmp_buffer)
{
  if (cancelled())
    return false;

  if (!is_directory)
  {
    FIL src{};
    const auto open_error_code = f_open(&src, name, FA_READ);
    if (open_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to open file %s in SD image: %s", path.c_str(),
                FatFsErrorToString(open_error_code));
      return false;
    }

    File::IOFile dst(path, "wb");
    if (!dst)
    {
      ERROR_LOG(COMMON, "Failed to open file %s", path.c_str());
      return false;
    }

    u32 size = f_size(&src);
    while (size > 0)
    {
      if (cancelled())
        return false;

      u32 chunk_size = size < tmp_buffer.size() ? size : static_cast<u32>(tmp_buffer.size());
      u32 read_size;
      const auto read_error_code = f_read(&src, tmp_buffer.data(), chunk_size, &read_size);
      if (read_error_code != FR_OK)
      {
        ERROR_LOG(COMMON, "Failed to read from file %s in SD image: %s", path.c_str(),
                  FatFsErrorToString(read_error_code));
        return false;
      }

      if (read_size != chunk_size)
      {
        ERROR_LOG(COMMON, "Failed to read bytes of file %s in SD image (%u != %u)", path.c_str(),
                  read_size, chunk_size);
        return false;
      }

      if (!dst.WriteBytes(tmp_buffer.data(), chunk_size))
      {
        ERROR_LOG(COMMON, "Failed to write to file %s", path.c_str());
        return false;
      }

      size -= chunk_size;
    }

    if (!dst.Close())
    {
      ERROR_LOG(COMMON, "Failed to close file %s", path.c_str());
      return false;
    }

    const auto close_error_code = f_close(&src);
    if (close_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to close file %s in SD image: %s", path.c_str(),
                FatFsErrorToString(close_error_code));
      return false;
    }

    return true;
  }

  if (!File::CreateDir(path))
  {
    ERROR_LOG(COMMON, "Failed to create directory %s", path.c_str());
    return false;
  }

  const auto chdir_error_code = f_chdir(name);
  if (chdir_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to enter directory %s in SD image: %s", path.c_str(),
              FatFsErrorToString(chdir_error_code));
    return false;
  }

  DIR directory{};
  const auto opendir_error_code = f_opendir(&directory, ".");
  if (opendir_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to open directory %s in SD image: %s", path.c_str(),
              FatFsErrorToString(opendir_error_code));
    return false;
  }

  FILINFO entry{};
  while (true)
  {
    const auto readdir_error_code = f_readdir(&directory, &entry);
    if (readdir_error_code != FR_OK)
    {
      ERROR_LOG(COMMON, "Failed to read directory %s in SD image: %s", path.c_str(),
                FatFsErrorToString(readdir_error_code));
      return false;
    }

    if (entry.fname[0] == '\0')
      break;

    if (entry.fname[0] == '?' && entry.fname[1] == '\0' && entry.altname[0] == '\0')
    {
      // FATFS indicates entries that have neither a short nor a long filename this way.
      // These are likely corrupted file entries so just skip them.
      continue;
    }

    const std::string_view childname = entry.fname;

    // Check for path traversal attacks.
    const bool is_path_traversal_attack =
        childname.contains('\\') || childname.contains('/') ||
        childname.find_first_not_of('.') == std::string::npos;
    if (is_path_traversal_attack)
    {
      ERROR_LOG(
          COMMON,
          "Path traversal attack detected in directory %s in SD image, child filename is %.*s",
          path.c_str(), static_cast<int>(childname.size()), childname.data());
      return false;
    }

    std::string child_path(path);
    child_path += '/';
    child_path.append(childname);
    if (!Unpack(cancelled, child_path, entry.fattrib & AM_DIR, entry.fname, tmp_buffer))
    {
      return false;
    }
  }

  const auto closedir_error_code = f_closedir(&directory);
  if (closedir_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to close directory %s in SD image: %s", path.c_str(),
              FatFsErrorToString(closedir_error_code));
    return false;
  }

  const auto chdir_up_error_code = f_chdir("..");
  if (chdir_up_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to leave directory %s in SD image: %s", path.c_str(),
              FatFsErrorToString(chdir_up_error_code));
    return false;
  }

  return true;
}

bool SyncSDImageToSDFolder(const std::function<bool()>& cancelled)
{
  const std::string image_path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  const std::string target_dir = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  if (image_path.empty() || target_dir.empty())
    return false;

  const FatFsUse fatfs_use;
  if (!fatfs_use)
    return false;
  SDCardFatFsCallbacks callbacks;
  s_callbacks = &callbacks;
  Common::ScopeGuard callbacks_guard{[] { s_callbacks = nullptr; }};

  INFO_LOG(COMMON, "Starting SD card conversion from file %s to folder %s", image_path.c_str(),
           target_dir.c_str());

  File::IOFile image;
  callbacks.m_image = &image;

  // this shouldn't matter since we're not modifying the SD image here, but initialize it to
  // something consistent just in case
  callbacks.m_deterministic = true;

  if (!image.Open(image_path, "r+b"))
  {
    ERROR_LOG(COMMON, "Failed to open SD image at %s", image_path.c_str());
    return false;
  }

  FATFS fs{};
  const auto mount_error_code = f_mount(&fs, "", 0);
  if (mount_error_code != FR_OK)
  {
    ERROR_LOG(COMMON, "Failed to mount SD image file system: %s",
              FatFsErrorToString(mount_error_code));
    return false;
  }
  Common::ScopeGuard unmount_guard{[] { f_unmount(""); }};

  // Unpack() and GetTempFilenameForAtomicWrite() don't want the trailing separator.
  const std::string target_dir_without_slash = target_dir.substr(0, target_dir.length() - 1);

  // Most systems don't offer atomic directory renaming, so it's simpler to directly work on the
  // actual one and rollback if needed.
  const bool target_dir_exists = File::IsDirectory(target_dir);
  const std::string backup_target_dir_without_slash =
      File::GetTempFilenameForAtomicWrite(target_dir_without_slash);

  if (target_dir_exists)
  {
    if (!File::Rename(target_dir_without_slash, backup_target_dir_without_slash))
    {
      ERROR_LOG(COMMON, "Failed to move old SD folder to %s",
                backup_target_dir_without_slash.c_str());
      return false;
    }
  }

  std::vector<u8> tmp_buffer(MAX_CLUSTER_SIZE);
  if (!Unpack(cancelled, target_dir_without_slash, true, "", tmp_buffer))
  {
    ERROR_LOG(COMMON, "Failed to unpack SD image %s to %s", image_path.c_str(), target_dir.c_str());
    File::DeleteDirRecursively(target_dir_without_slash);
    if (target_dir_exists)
      File::Rename(backup_target_dir_without_slash, target_dir_without_slash);
    return false;
  }

  unmount_guard.Exit();  // unmount before closing the image

  if (target_dir_exists)
    File::DeleteDirRecursively(backup_target_dir_without_slash);

  // even if this fails the conversion has already succeeded, so we still return true
  if (!image.Close())
    ERROR_LOG(COMMON, "Failed to close SD image %s", image_path.c_str());

  INFO_LOG(COMMON, "Successfully unpacked SD image %s to %s", image_path.c_str(),
           target_dir.c_str());
  return true;
}

void RunInFatFsContext(FatFsCallbacks& callbacks, const std::function<void()>& function)
{
  const FatFsUse fatfs_use;
  if (!fatfs_use)
    return;
  s_callbacks = &callbacks;
  Common::ScopeGuard callbacks_guard{[] { s_callbacks = nullptr; }};
  function();
}
}  // namespace Common
