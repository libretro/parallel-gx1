// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/FileSystem.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <retro_miscellaneous.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/FileSystem.h"

namespace IOS::HLE::FS
{
bool IsValidPath(const char* path)
{
  return !std::strcmp(path, "/") || IsValidNonRootPath(path);
}

bool IsValidNonRootPath(const char* path)
{
  // one past the longest valid length is enough to tell
  const size_t length = strnlen(path, MaxPathLength + 1);
  return length > 1 && length <= MaxPathLength && path[0] == '/' && path[length - 1] != '/';
}

void FreeDirectoryList(DirectoryList* list)
{
  std::free(list->names);
  list->names = nullptr;
  list->count = 0;
}

void SplitPathAndBasename(const char* path, SplitPathResult* out)
{
  const char* const last_separator = std::strrchr(path, '/');
  size_t parent_length = last_separator ? size_t(last_separator - path) : 0;
  if (parent_length < 1)
    parent_length = 1;
  if (parent_length > MaxPathLength)
    parent_length = MaxPathLength;
  std::memcpy(out->parent, path, parent_length);
  out->parent[parent_length] = '\0';

  const char* const file_name = last_separator ? last_separator + 1 : path;
  const size_t file_name_length = strnlen(file_name, MaxPathLength);
  std::memcpy(out->file_name, file_name, file_name_length);
  out->file_name[file_name_length] = '\0';
}

FileSystem* MakeFileSystem(Location location)
{
  return new FileSystem(
      File::GetUserPath(location == Location::Session ? D_SESSION_WIIROOT_IDX : D_WIIROOT_IDX)
          .c_str());
}

IOS::HLE::ReturnCode ConvertResult(ResultCode code)
{
  if (code == ResultCode::Success)
    return IPC_SUCCESS;
  // FS error codes start at -100. Since result codes in the enum are listed in the same way
  // as the IOS codes, we just need to return -100-code.
  return static_cast<ReturnCode>(-(static_cast<s32>(code) + 100));
}

FileHandle::FileHandle(FileSystem* fs, Fd fd) : m_fs{fs}, m_fd{fd}, m_has_fd{true}
{
}

FileHandle::FileHandle(FileHandle&& other)
    : m_fs{other.m_fs}, m_fd{other.m_fd}, m_has_fd{other.m_has_fd}
{
  other.m_has_fd = false;
}

FileHandle& FileHandle::operator=(FileHandle&& other)
{
  if (this != &other)
  {
    if (m_has_fd && m_fs)
      ASSERT(m_fs->Close(m_fd) == FS::ResultCode::Success);
    m_fs = other.m_fs;
    m_fd = other.m_fd;
    m_has_fd = other.m_has_fd;
    other.m_has_fd = false;
  }
  return *this;
}

FileHandle::~FileHandle()
{
  if (m_has_fd && m_fs)
    ASSERT(m_fs->Close(m_fd) == FS::ResultCode::Success);
}

Fd FileHandle::Release()
{
  ASSERT(m_has_fd);
  m_has_fd = false;
  return m_fd;
}

Result<u32> FileHandle::Seek(u32 offset, SeekMode mode) const
{
  return m_fs->SeekFile(m_fd, offset, mode);
}

Result<FileStatus> FileHandle::GetStatus() const
{
  return m_fs->GetFileStatus(m_fd);
}

Result<FileHandle> FileSystem::CreateAndOpenFile(Uid uid, Gid gid, const char* path, Modes modes)
{
  if (Result<FileHandle> file = OpenFile(uid, gid, path, Mode::ReadWrite))
    return file;

  const ResultCode result = CreateFile(uid, gid, path, 0, modes);
  if (result != ResultCode::Success)
    return std::unexpected{result};

  return OpenFile(uid, gid, path, Mode::ReadWrite);
}

ResultCode FileSystem::CreateFullPath(Uid uid, Gid gid, const char* path, FileAttribute attribute,
                                      Modes modes)
{
  const char* separator = path[0] ? path + 1 : path;
  while (true)
  {
    separator = std::strchr(separator, '/');
    if (!separator)
      return ResultCode::Success;

    // A parent longer than a path can be is not a valid path
    const size_t length = size_t(separator - path);
    if (length > MaxPathLength)
      return ResultCode::Invalid;
    char subpath[MaxPathLength + 1];
    std::memcpy(subpath, path, length);
    subpath[length] = '\0';
    Metadata metadata;
    const ResultCode metadata_result = GetMetadata(uid, gid, subpath, &metadata);
    if (metadata_result != ResultCode::Success && metadata_result != ResultCode::NotFound)
      return metadata_result;
    if (metadata_result == ResultCode::Success && metadata.is_file)
      return ResultCode::Invalid;

    if (metadata_result != ResultCode::Success)
    {
      const ResultCode result = CreateDirectory(uid, gid, subpath, attribute, modes);
      if (result != ResultCode::Success)
        return result;
    }

    ++separator;
  }
}

// Names are saved as strings were: their length, then their characters.
static bool DoStateName(PointerWrap& p, char (&name)[MaxFilenameLength + 1])
{
  u32 length = static_cast<u32>(std::strlen(name));
  p.Do(length);
  if (length > MaxFilenameLength)
    return false;
  p.DoArray(name, length);
  name[length] = '\0';
  return true;
}

static bool BuildChildPath(const char* directory_path, const char* name,
                           char (&child_path)[MaxPathLength + 1])
{
  const int length = std::snprintf(child_path, sizeof(child_path), "%s/%s", directory_path, name);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(child_path))
  {
    ERROR_LOG(IOS_FS, "Savestate path %s/%s is too long", directory_path, name);
    return false;
  }
  return true;
}

void FileSystem::DoStateRead(PointerWrap& p, const char* directory_path, u8* buffer)
{
  const ResultCode delete_result = Delete(0, 0, directory_path);
  if (delete_result != ResultCode::Success && delete_result != ResultCode::NotFound)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call Delete for %s: %u", directory_path,
              static_cast<unsigned>(delete_result));
    p.SetVerifyMode();
    return;
  }

  Metadata metadata{};
  p.Do(metadata.uid);
  p.Do(metadata.gid);
  p.Do(metadata.attribute);
  p.Do(metadata.modes);
  p.Do(metadata.is_file);
  p.Do(metadata.size);
  p.Do(metadata.fst_index);

  // UID 0 is used to avoid AccessDenied.
  const ResultCode create_directory_result =
      CreateDirectory(0, 0, directory_path, metadata.attribute, metadata.modes);
  if (create_directory_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call CreateDirectory for %s: %u", directory_path,
              static_cast<unsigned>(create_directory_result));
    p.SetVerifyMode();
    return;
  }

  // Change the UID and GID from 0 to the intended values.
  const ResultCode set_metadata_result = SetMetadata(0, directory_path, metadata.uid, metadata.gid,
                                                     metadata.attribute, metadata.modes);
  if (set_metadata_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call SetMetadata for %s: %u", directory_path,
              static_cast<unsigned>(set_metadata_result));
    p.SetVerifyMode();
    return;
  }

  // Now restore from the stream
  u32 child_count = 0;
  p.Do(child_count);
  for (u32 child = 0; child < child_count && p.IsReadMode(); child++)
  {
    Metadata child_metadata;
    char child_name[MaxFilenameLength + 1] = "";
    char child_path[MaxPathLength + 1];
    p.Do(child_metadata);
    if (!DoStateName(p, child_name) || !BuildChildPath(directory_path, child_name, child_path))
    {
      p.SetVerifyMode();
      break;
    }

    if (child_metadata.is_file)
    {
      // UID 0 is used to avoid AccessDenied.
      const ResultCode create_file_result =
          CreateFile(0, 0, child_path, child_metadata.attribute, child_metadata.modes);
      if (create_file_result != ResultCode::Success)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call CreateFile for %s: %u", child_path,
                  static_cast<unsigned>(create_file_result));
        p.SetVerifyMode();
        continue;
      }

      // Change the UID and GID from 0 to the intended values.
      const ResultCode child_metadata_result =
          SetMetadata(0, child_path, child_metadata.uid, child_metadata.gid,
                      child_metadata.attribute, child_metadata.modes);
      if (child_metadata_result != ResultCode::Success)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call SetMetadata for %s: %u", child_path,
                  static_cast<unsigned>(child_metadata_result));
        p.SetVerifyMode();
        continue;
      }

      Result<FileHandle> handle = OpenFile(0, 0, child_path, Mode::Write);
      if (!handle)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call OpenFile for %s: %u", child_path,
                  static_cast<unsigned>(handle.error()));
        p.SetVerifyMode();
        continue;
      }

      u32 i = 0;
      while (i < child_metadata.size)
      {
        const u32 bytes_to_write = std::min(child_metadata.size - i, STATE_CHUNK_SIZE);
        p.DoArray(buffer, bytes_to_write);

        Result<size_t> write_result = handle->Write(buffer, bytes_to_write);
        if (!write_result)
        {
          ERROR_LOG(IOS_FS, "DoStateRead failed to call Write for %s: %u", child_path,
                    static_cast<unsigned>(write_result.error()));
          p.SetVerifyMode();
          break;
        }
        if (*write_result != bytes_to_write)
        {
          ERROR_LOG(IOS_FS, "DoStateRead tried to write %s bytes to %u but wrote %" PRIu64 " bytes",
                    child_path, bytes_to_write, (u64)(*write_result));
          p.SetVerifyMode();
          break;
        }

        i += bytes_to_write;
      }
    }
    else
    {
      DoStateRead(p, child_path, buffer);
    }
  }
}

void FileSystem::DoStateWriteOrMeasure(PointerWrap& p, const char* directory_path, u8* buffer)
{
  Metadata metadata;
  const ResultCode metadata_result = GetMetadata(0, 0, directory_path, &metadata);
  if (metadata_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call GetMetadata for %s: %u", directory_path,
              static_cast<unsigned>(metadata_result));
    p.SetVerifyMode();
    return;
  }
  p.Do(metadata.uid);
  p.Do(metadata.gid);
  p.Do(metadata.attribute);
  p.Do(metadata.modes);
  p.Do(metadata.is_file);
  p.Do(metadata.size);
  p.Do(metadata.fst_index);

  DirectoryList children;
  const ResultCode list_result = ReadDirectory(0, 0, directory_path, &children);
  if (list_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call ReadDirectory for %s: %u",
              directory_path, static_cast<unsigned>(list_result));
    p.SetVerifyMode();
    return;
  }

  u32 child_count = children.count;
  p.Do(child_count);
  for (u32 child = 0; child < child_count; child++)
  {
    char child_path[MaxPathLength + 1];
    char* const child_name = children.names[child];
    if (!BuildChildPath(directory_path, child_name, child_path))
    {
      p.SetVerifyMode();
      continue;
    }

    Metadata child_metadata;
    const ResultCode child_result = GetMetadata(0, 0, child_path, &child_metadata);
    if (child_result != ResultCode::Success)
    {
      ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call GetMetadata for %s: %u", child_name,
                static_cast<unsigned>(child_result));
      p.SetVerifyMode();
      continue;
    }

    p.Do(child_metadata);
    DoStateName(p, children.names[child]);

    if (child_metadata.is_file)
    {
      Result<FileHandle> handle = OpenFile(0, 0, child_path, Mode::Read);
      if (!handle)
      {
        ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call OpenFile for %s: %u", child_name,
                  static_cast<unsigned>(handle.error()));
        p.SetVerifyMode();
        continue;
      }

      u32 i = 0;
      while (i < child_metadata.size)
      {
        const u32 bytes_to_read = std::min(child_metadata.size - i, STATE_CHUNK_SIZE);
        Result<size_t> read_result = handle->Read(buffer, bytes_to_read);
        if (!read_result)
        {
          ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call Read for %s: %u", child_name,
                    static_cast<unsigned>(read_result.error()));
          p.SetVerifyMode();
          break;
        }
        if (*read_result != bytes_to_read)
        {
          ERROR_LOG(IOS_FS,
                    "DoStateWriteOrMeasure tried to read %s bytes from %u but got %" PRIu64
                    " bytes",
                    child_name, bytes_to_read, (u64)(*read_result));
          p.SetVerifyMode();
          break;
        }

        p.DoArray(buffer, bytes_to_read);
        i += bytes_to_read;
      }
    }
    else
    {
      DoStateWriteOrMeasure(p, child_path, buffer);
    }
  }
  FreeDirectoryList(&children);
}
}  // namespace IOS::HLE::FS
