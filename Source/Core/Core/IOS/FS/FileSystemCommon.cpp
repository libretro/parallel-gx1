// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/FileSystem.h"

#include <algorithm>
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
constexpr u32 BUFFER_CHUNK_SIZE = 65536;

bool IsValidPath(std::string_view path)
{
  return path == "/" || IsValidNonRootPath(path);
}

bool IsValidNonRootPath(std::string_view path)
{
  return path.length() > 1 && path.length() <= MaxPathLength && path[0] == '/' &&
         path.back() != '/';
}

bool IsValidFilename(std::string_view filename)
{
  return filename.length() <= MaxFilenameLength &&
         !std::ranges::any_of(filename, [](char c) { return c == '/'; });
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
  const std::string nand_root =
      File::GetUserPath(location == Location::Session ? D_SESSION_WIIROOT_IDX : D_WIIROOT_IDX);
  return new FileSystem(nand_root);
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
    const Result<Metadata> metadata = GetMetadata(uid, gid, subpath);
    if (!metadata && metadata.error() != ResultCode::NotFound)
      return metadata.error();
    if (metadata && metadata->is_file)
      return ResultCode::Invalid;

    if (!metadata)
    {
      const ResultCode result = CreateDirectory(uid, gid, subpath, attribute, modes);
      if (result != ResultCode::Success)
        return result;
    }

    ++separator;
  }
}

void FileSystem::DoStateRead(PointerWrap& p, const std::string& directory_path)
{
  const ResultCode delete_result = Delete(0, 0, directory_path.c_str());
  if (delete_result != ResultCode::Success && delete_result != ResultCode::NotFound)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call Delete for %s: %u", directory_path.c_str(),
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
      CreateDirectory(0, 0, directory_path.c_str(), metadata.attribute, metadata.modes);
  if (create_directory_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call CreateDirectory for %s: %u",
              directory_path.c_str(), static_cast<unsigned>(create_directory_result));
    p.SetVerifyMode();
    return;
  }

  // Change the UID and GID from 0 to the intended values.
  const ResultCode set_metadata_result = SetMetadata(
      0, directory_path.c_str(), metadata.uid, metadata.gid, metadata.attribute, metadata.modes);
  if (set_metadata_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateRead failed to call SetMetadata for %s: %u", directory_path.c_str(),
              static_cast<unsigned>(set_metadata_result));
    p.SetVerifyMode();
    return;
  }

  // Now restore from the stream
  std::vector<std::string> children;
  p.DoEachElement(children, [this, &directory_path](PointerWrap& p_, std::string& child_name) {
    Metadata child_metadata;
    p_.Do(child_metadata);
    p_.Do(child_name);

    std::string child_path;
    child_path.reserve(directory_path.size() + child_name.size() + 1);
    child_path.append(directory_path);
    if (directory_path.back() != '/')
      child_path.push_back('/');
    child_path.append(child_name);

    if (child_metadata.is_file)
    {
      // UID 0 is used to avoid AccessDenied.
      const ResultCode create_file_result =
          CreateFile(0, 0, child_path.c_str(), child_metadata.attribute, child_metadata.modes);
      if (create_file_result != ResultCode::Success)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call CreateFile for %s: %u", child_path.c_str(),
                  static_cast<unsigned>(create_file_result));
        p_.SetVerifyMode();
        return;
      }

      // Change the UID and GID from 0 to the intended values.
      const ResultCode child_metadata_result =
          SetMetadata(0, child_path.c_str(), child_metadata.uid, child_metadata.gid,
                      child_metadata.attribute, child_metadata.modes);
      if (child_metadata_result != ResultCode::Success)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call SetMetadata for %s: %u", child_path.c_str(),
                  static_cast<unsigned>(child_metadata_result));
        p_.SetVerifyMode();
        return;
      }

      u8 buffer[BUFFER_CHUNK_SIZE];
      Result<FileHandle> handle = OpenFile(0, 0, child_path.c_str(), Mode::Write);
      if (!handle)
      {
        ERROR_LOG(IOS_FS, "DoStateRead failed to call OpenFile for %s: %u", child_path.c_str(),
                  static_cast<unsigned>(handle.error()));
        p_.SetVerifyMode();
        return;
      }

      u32 i = 0;
      while (i < child_metadata.size)
      {
        const u32 bytes_to_write =
            std::min(child_metadata.size - i, static_cast<u32>(ARRAY_SIZE(buffer)));
        p_.DoArray(buffer, bytes_to_write);

        Result<size_t> write_result = handle->Write(buffer, bytes_to_write);
        if (!write_result)
        {
          ERROR_LOG(IOS_FS, "DoStateRead failed to call Write for %s: %u", child_path.c_str(),
                    static_cast<unsigned>(write_result.error()));
          p_.SetVerifyMode();
          return;
        }
        if (*write_result != bytes_to_write)
        {
          ERROR_LOG(IOS_FS, "DoStateRead tried to write %s bytes to %u but wrote %" PRIu64 " bytes",
                    child_path.c_str(), bytes_to_write, (u64)(*write_result));
          p_.SetVerifyMode();
          return;
        }

        i += bytes_to_write;
      }
    }
    else
    {
      DoStateRead(p_, child_path);
    }
  });
}

void FileSystem::DoStateWriteOrMeasure(PointerWrap& p, const std::string& directory_path)
{
  const Result<Metadata> metadata = GetMetadata(0, 0, directory_path.c_str());
  if (!metadata)
  {
    ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call GetMetadata for %s: %u",
              directory_path.c_str(), static_cast<unsigned>(metadata.error()));
    p.SetVerifyMode();
    return;
  }
  p.Do(metadata->uid);
  p.Do(metadata->gid);
  p.Do(metadata->attribute);
  p.Do(metadata->modes);
  p.Do(metadata->is_file);
  p.Do(metadata->size);
  p.Do(metadata->fst_index);

  DirectoryList children;
  const ResultCode list_result = ReadDirectory(0, 0, directory_path.c_str(), &children);
  if (list_result != ResultCode::Success)
  {
    ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call ReadDirectory for %s: %u",
              directory_path.c_str(), static_cast<unsigned>(list_result));
    p.SetVerifyMode();
    return;
  }

  u32 child_count = children.count;
  p.Do(child_count);
  for (u32 child = 0; child < child_count; child++)
  {
    std::string child_name = children.names[child];
    std::string child_path;
    child_path.reserve(directory_path.size() + child_name.size() + 1);
    child_path.append(directory_path);
    if (directory_path.back() != '/')
      child_path.push_back('/');
    child_path.append(child_name);

    Result<Metadata> child_metadata = GetMetadata(0, 0, child_path.c_str());
    if (!child_metadata)
    {
      ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call GetMetadata for %s: %u",
                child_name.c_str(), static_cast<unsigned>(child_metadata.error()));
      p.SetVerifyMode();
      continue;
    }

    p.Do(*child_metadata);
    p.Do(child_name);

    if (child_metadata->is_file)
    {
      u8 buffer[BUFFER_CHUNK_SIZE];
      Result<FileHandle> handle = OpenFile(0, 0, child_path.c_str(), Mode::Read);
      if (!handle)
      {
        ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call OpenFile for %s: %u",
                  child_name.c_str(), static_cast<unsigned>(handle.error()));
        p.SetVerifyMode();
        continue;
      }

      u32 i = 0;
      while (i < child_metadata->size)
      {
        const u32 bytes_to_read =
            std::min(child_metadata->size - i, static_cast<u32>(ARRAY_SIZE(buffer)));
        Result<size_t> read_result = handle->Read(buffer, bytes_to_read);
        if (!read_result)
        {
          ERROR_LOG(IOS_FS, "DoStateWriteOrMeasure failed to call Read for %s: %u",
                    child_name.c_str(), static_cast<unsigned>(read_result.error()));
          p.SetVerifyMode();
          break;
        }
        if (*read_result != bytes_to_read)
        {
          ERROR_LOG(IOS_FS,
                    "DoStateWriteOrMeasure tried to read %s bytes from %u but got %" PRIu64
                    " bytes",
                    child_name.c_str(), bytes_to_read, (u64)(*read_result));
          p.SetVerifyMode();
          break;
        }

        p.DoArray(buffer, bytes_to_read);
        i += bytes_to_read;
      }
    }
    else
    {
      DoStateWriteOrMeasure(p, child_path);
    }
  }
  FreeDirectoryList(&children);
}
}  // namespace IOS::HLE::FS
