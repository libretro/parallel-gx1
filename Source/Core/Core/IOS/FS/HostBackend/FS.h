// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/FS/FileSystem.h"

namespace IOS::HLE::FS
{
/// Backend that uses the host file system as backend.
///
/// Ignores metadata like permissions, attributes and various checks and also
/// sometimes returns wrong information because metadata is not available.
class HostFileSystem final : public FileSystem
{
public:
  explicit HostFileSystem(std::string root_path);
  ~HostFileSystem() override;

  void DoState(PointerWrap& p) override;

  ResultCode Format(Uid uid) override;

  Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) override;
  ResultCode Close(Fd fd) override;
  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) override;
  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) override;
  Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode) override;
  Result<FileStatus> GetFileStatus(Fd fd) override;

  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes) override;

  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes) override;

  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path) override;

  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path) override;

  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes) override;

  Result<NandStats> GetNandStats() override;
  Result<DirectoryStats> GetDirectoryStats(const std::string& path) override;
  Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path) override;


private:
  struct FstEntry
  {
    bool CheckPermission(Uid uid, Gid gid, Mode requested_mode) const;
    std::string name;
    Metadata data{};
    /// Children of this FST entry. Only valid for directories.
    ///
    /// We use a vector rather than a list here because iterating over children
    /// happens a lot more often than removals.
    /// Newly created entries are added at the end.
    std::vector<FstEntry> children;
  };

  struct Handle
  {
    bool opened = false;
    Mode mode = Mode::None;
    std::string wii_path;
    /// index into m_host_files, -1 when none
    int host_file = -1;
    u32 file_offset = 0;
  };
  /// One host file shared by every guest handle open on the same path, so all
  /// accesses to it are strongly ordered like on the Wii.
  struct HostFile
  {
    File::IOFile file;
    std::string host_path;
    u32 refs = 0;
  };
  enum
  {
    NUM_HANDLES = 16
  };
  Handle* AssignFreeHandle();
  File::IOFile* GetHostFile(const Handle* handle);
  void CloseHandle(Handle* handle);
  void CloseAllHandles();
  Handle* GetHandleFromFd(Fd fd);
  Fd ConvertHandleToFd(const Handle* handle) const;

  std::string BuildFilename(const std::string& wii_path) const;
  int OpenHostFile(const std::string& host_path);
  void ReleaseHostFile(int index);

  ResultCode CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                   FileAttribute attribute, Modes modes, bool is_file);
  bool IsFileOpened(const std::string& path) const;
  bool IsDirectoryInUse(const std::string& path) const;

  std::string GetFstFilePath() const;
  void ResetFst();
  void LoadFst();
  void SaveFst();
  /// Get the FST entry for a file (or directory).
  /// Automatically creates fallback entries for parents if they do not exist.
  /// Returns nullptr if the path is invalid or the file does not exist.
  FstEntry* GetFstEntryForPath(const std::string& path);

  /// FST entry for the filesystem root.
  ///
  /// Note that unlike a real Wii's FST, ours is the single source of truth only for
  /// filesystem metadata and ordering. File existence must be checked by querying
  /// the host filesystem.
  /// The reasons for this design are twofold: existing users do not have a FST
  /// and we do not want FS to break if the user adds or removes files in their
  /// filesystem root manually.
  FstEntry m_root_entry{};
  std::string m_root_path;
  HostFile m_host_files[NUM_HANDLES];
  Handle m_handles[NUM_HANDLES];
};

}  // namespace IOS::HLE::FS
