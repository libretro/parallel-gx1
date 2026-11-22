// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
// TODO: Horrible hack, remove ASAP!
#include <windows.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

class PointerWrap;

namespace IOS::HLE
{
enum ReturnCode : s32;

namespace FS
{
enum class ResultCode
{
  Success,
  Invalid,
  AccessDenied,
  SuperblockWriteFailed,
  SuperblockInitFailed,
  AlreadyExists,
  NotFound,
  FstFull,
  NoFreeSpace,
  NoFreeHandle,
  TooManyPathComponents,
  InUse,
  BadBlock,
  EccError,
  CriticalEccError,
  FileNotEmpty,
  CheckFailed,
  UnknownError,
  ShortRead,
};

template <typename T>
using Result = std::expected<T, ResultCode>;

using Uid = u32;
using Gid = u16;
using Fd = u32;

enum class Mode : u8
{
  None = 0,
  Read = 1,
  Write = 2,
  ReadWrite = 3,
};

enum class SeekMode : u32
{
  Set = 0,
  Current = 1,
  End = 2,
};

using FileAttribute = u8;

struct Modes
{
  Mode owner, group, other;
};
inline bool operator==(const Modes& lhs, const Modes& rhs)
{
  const auto fields = [](const Modes& obj) { return std::tie(obj.owner, obj.group, obj.other); };
  return fields(lhs) == fields(rhs);
}

struct Metadata
{
  Uid uid;
  Gid gid;
  FileAttribute attribute;
  Modes modes;
  bool is_file;
  u32 size;
  u16 fst_index;
};

// size of a single cluster in the NAND in bytes
constexpr u16 CLUSTER_SIZE = 16384;

// total number of clusters available in the NAND
constexpr u16 TOTAL_CLUSTERS = 0x7ec0;

// number of clusters reserved for bad blocks and similar, not accessible to normal writes
constexpr u16 RESERVED_CLUSTERS = 0x0300;

// number of clusters actually usable by the file system
constexpr u16 USABLE_CLUSTERS = TOTAL_CLUSTERS - RESERVED_CLUSTERS;

// size of a single 'block' as defined by the Wii System Menu in clusters
constexpr u16 CLUSTERS_PER_BLOCK = 8;

// total number of user-accessible blocks in the NAND
constexpr u16 USER_BLOCKS = 2176;

// total number of user-accessible clusters in the NAND
constexpr u16 USER_CLUSTERS = USER_BLOCKS * CLUSTERS_PER_BLOCK;

// the inverse of that, the amount of usable clusters reserved for system files
constexpr u16 SYSTEM_CLUSTERS = USABLE_CLUSTERS - USER_CLUSTERS;

// total number of inodes available in the NAND
constexpr u16 TOTAL_INODES = 0x17ff;

struct NandStats
{
  u32 cluster_size;
  u32 free_clusters;
  u32 used_clusters;
  u32 bad_clusters;
  u32 reserved_clusters;
  u32 free_inodes;
  u32 used_inodes;
};

struct DirectoryStats
{
  u32 used_clusters;
  u32 used_inodes;
};

// Not a real Wii data struct, but useful for calculating how full the user's NAND is even if it's
// way larger than it should be.
struct ExtendedDirectoryStats
{
  u64 used_clusters;
  u64 used_inodes;
};

struct FileStatus
{
  u32 offset;
  u32 size;
};

/// The maximum number of components a path can have.
constexpr size_t MaxPathDepth = 8;
/// The maximum number of characters a path can have.
constexpr size_t MaxPathLength = 64;
/// The maximum number of characters a filename can have.
constexpr size_t MaxFilenameLength = 12;

/// Returns whether a Wii path is valid.
bool IsValidPath(std::string_view path);
bool IsValidNonRootPath(std::string_view path);
bool IsValidFilename(std::string_view filename);

struct SplitPathResult
{
  std::string parent;
  std::string file_name;
};
inline bool operator==(const SplitPathResult& lhs, const SplitPathResult& rhs)
{
  const auto fields = [](const SplitPathResult& obj) {
    return std::tie(obj.parent, obj.file_name);
  };
  return fields(lhs) == fields(rhs);
}

/// Split a path into a parent path and the file name. Takes a *valid non-root* path.
///
/// Example: /shared2/sys/SYSCONF => {/shared2/sys, SYSCONF}
SplitPathResult SplitPathAndBasename(std::string_view path);

class FileSystem;
class FileHandle final
{
public:
  FileHandle(FileSystem* fs, Fd fd);
  FileHandle(FileHandle&&);
  ~FileHandle();
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle& operator=(FileHandle&&);

  /// Release the FD so that it is not automatically closed.
  Fd Release();

  template <typename T>
  Result<size_t> Read(T* ptr, size_t count) const;

  template <typename T>
  Result<size_t> Write(const T* ptr, size_t count) const;

  Result<u32> Seek(u32 offset, SeekMode mode) const;
  Result<FileStatus> GetStatus() const;

private:
  FileSystem* m_fs;
  std::optional<Fd> m_fd;
};

class FileSystem
{
public:
  /// The host's file system, rooted at root_path, as the NAND.
  ///
  /// Ignores metadata like permissions, attributes and various checks and also
  /// sometimes returns wrong information because metadata is not available.
  explicit FileSystem(std::string root_path);
  ~FileSystem();

  void DoState(PointerWrap& p);

  /// Format the file system.
  ResultCode Format(Uid uid);

  /// Get a file descriptor for accessing a file. The FD will be automatically closed after use.
  Result<FileHandle> OpenFile(Uid uid, Gid gid, const char* path, Mode mode);
  /// Create a file if it doesn't exist and open it in read/write mode.
  Result<FileHandle> CreateAndOpenFile(Uid uid, Gid gid, const std::string& path, Modes modes);
  /// Close a file descriptor.
  ResultCode Close(Fd fd);
  /// Read `size` bytes from the file descriptor. Returns the number of bytes read.
  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size);
  /// Write `size` bytes to the file descriptor. Returns the number of bytes written.
  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size);
  /// Reposition the file offset for a file descriptor.
  Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode);
  /// Get status for a file descriptor.
  /// Guaranteed to succeed for a valid file descriptor.
  Result<FileStatus> GetFileStatus(Fd fd);

  /// Create a file with the specified path and metadata.
  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes);
  /// Create a directory with the specified path and metadata.
  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes);

  /// Create any parent directories for a path with the specified metadata.
  /// Example: "/a/b" to create directory /a; "/a/b/" to create directories /a and /a/b
  ResultCode CreateFullPath(Uid caller_uid, Gid caller_gid, const std::string& path,
                            FileAttribute attribute, Modes modes);

  /// Delete a file or directory with the specified path.
  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path);
  /// Rename a file or directory with the specified path.
  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path);

  /// List the children of a directory (non-recursively).
  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path);

  /// Get metadata about a file.
  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path);
  /// Set metadata for a file.
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes);

  /// Get usage information about the NAND (block size, cluster and inode counts).
  Result<NandStats> GetNandStats();
  /// Get usage information about a directory (used cluster and inode counts).
  Result<DirectoryStats> GetDirectoryStats(const std::string& path);

  /// Like GetDirectoryStats() but not limited to the actual 512 MB NAND limit.
  Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path);

protected:
  void DoStateWriteOrMeasure(PointerWrap& p, const std::string& directory_path);
  void DoStateRead(PointerWrap& p, const std::string& directory_path);

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

  enum
  {
    // Longer than any path the FS takes: a longer one does not open.
    HANDLE_PATH_SIZE = 256,
    // A host path: the root and an escaped Wii path.
    HOST_PATH_SIZE = 2048
  };

  struct Handle
  {
    bool opened = false;
    Mode mode = Mode::None;
    char wii_path[HANDLE_PATH_SIZE] = {};
    /// index into m_host_files, -1 when none
    int host_file = -1;
    u32 file_offset = 0;
  };
  /// One host file shared by every guest handle open on the same path, so all
  /// accesses to it are strongly ordered like on the Wii.
  struct HostFile
  {
    File::IOFile file;
    char host_path[HOST_PATH_SIZE] = {};
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
  // Writes the host path of wii_path to out; returns false when it does not fit.
  bool BuildFilename(const char* wii_path, char* out, size_t out_size) const;
  int OpenHostFile(const char* host_path);
  void ReleaseHostFile(int index);

  ResultCode CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                   FileAttribute attribute, Modes modes, bool is_file);
  bool IsFileOpened(const char* path) const;
  bool IsDirectoryInUse(const char* path) const;

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

template <typename T>
Result<size_t> FileHandle::Read(T* ptr, size_t count) const
{
  const Result<u32> bytes = m_fs->ReadBytesFromFile(*m_fd, reinterpret_cast<u8*>(ptr),
                                                    static_cast<u32>(sizeof(T) * count));
  if (!bytes)
    return bytes;
  if (*bytes != sizeof(T) * count)
    return std::unexpected{ResultCode::ShortRead};
  return count;
}

template <typename T>
Result<size_t> FileHandle::Write(const T* ptr, size_t count) const
{
  const auto result = m_fs->WriteBytesToFile(*m_fd, reinterpret_cast<const u8*>(ptr),
                                             static_cast<u32>(sizeof(T) * count));
  if (!result)
    return result;
  return count;
}

enum class Location
{
  Configured,
  Session,
};

// Freed with delete
FileSystem* MakeFileSystem(Location location = Location::Session);

/// Convert a FS result code to an IOS error code.
IOS::HLE::ReturnCode ConvertResult(ResultCode code);

}  // namespace FS
}  // namespace IOS::HLE
