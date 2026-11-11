// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/HostBackend/FS.h"

#include <expected>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace IOS::HLE::FS
{
// This isn't theadsafe, but it's only called from the CPU thread.
int HostFileSystem::OpenHostFile(const std::string& host_path)
{
  // On the wii, all file operations are strongly ordered.
  // If a game opens the same file twice (or 8 times, looking at you PokePark Wii)
  // and writes to one file handle, it will be able to immediately read the written
  // data from the other handle.
  // On 'real' operating systems, there are various buffers and caches meaning
  // applications doing such naughty things will not get expected results.

  // So we fix this by catching any attempts to open the same file twice and
  // only opening one file. Accesses to a single file handle are ordered.
  //
  // Hall of Shame:
  //    - PokePark Wii (gets stuck on the loading screen of Pikachu falling)
  //    - PokePark 2 (Also gets stuck while loading)
  //    - Wii System Menu (Can't access the system settings, gets stuck on blank screen)
  //    - The Beatles: Rock Band (saving doesn't work)
  int free_index = -1;
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
  {
    HostFile& host_file = m_host_files[i];
    if (!host_file.refs)
    {
      if (free_index < 0)
        free_index = i;
      continue;
    }
    if (host_file.host_path == host_path)
    {
      host_file.refs++;
      return i;
    }
  }

  // one host file per guest handle at most, so a free slot always exists
  if (free_index < 0)
    return -1;

  // All files are opened read/write. Actual access rights will be controlled per handle by the
  // read/write functions below
  HostFile& host_file = m_host_files[free_index];
  while (!host_file.file.Open(host_path, "r+b"))
  {
    const bool try_again =
        PanicYesNo("File \"%s\" could not be opened!\n"
                   "This may happen with improper permissions or use by another process.\n"
                   "Press \"Yes\" to make another attempt.",
                   host_path.c_str());

    if (!try_again)
    {
      // We've failed to open the file:
      ERROR_LOG(IOS_FS, "OpenHostFile %s", host_path.c_str());
      return -1;
    }
  }

  host_file.host_path = host_path;
  host_file.refs = 1;
  return free_index;
}

void HostFileSystem::ReleaseHostFile(int index)
{
  if (index < 0 || index >= NUM_HANDLES || !m_host_files[index].refs)
    return;
  HostFile& host_file = m_host_files[index];
  // the last guest handle closes the host file
  if (--host_file.refs)
    return;
  host_file.file.Close();
  host_file.host_path.clear();
}

File::IOFile* HostFileSystem::GetHostFile(const Handle* handle)
{
  if (handle->host_file < 0)
    return nullptr;
  return &m_host_files[handle->host_file].file;
}

void HostFileSystem::CloseHandle(Handle* handle)
{
  ReleaseHostFile(handle->host_file);
  *handle = Handle{};
}

void HostFileSystem::CloseAllHandles()
{
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
    CloseHandle(&m_handles[i]);
}

Result<FileHandle> HostFileSystem::OpenFile(Uid, Gid, const std::string& path, Mode mode)
{
  Handle* handle = AssignFreeHandle();
  if (!handle)
    return std::unexpected{ResultCode::NoFreeHandle};

  const std::string host_path = BuildFilename(path);
  if (File::IsDirectory(host_path))
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::Invalid};
  }

  if (!File::IsFile(host_path))
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::NotFound};
  }

  handle->host_file = OpenHostFile(host_path);
  if (handle->host_file < 0)
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::AccessDenied};
  }

  handle->wii_path = path;
  handle->mode = mode;
  handle->file_offset = 0;
  return FileHandle{this, ConvertHandleToFd(handle)};
}

ResultCode HostFileSystem::Close(Fd fd)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle)
    return ResultCode::Invalid;

  // the host file closes with its last handle
  CloseHandle(handle);
  return ResultCode::Success;
}

Result<u32> HostFileSystem::ReadBytesFromFile(Fd fd, u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  File::IOFile* const host_file = handle ? GetHostFile(handle) : nullptr;
  if (!host_file || !host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  if ((u8(handle->mode) & u8(Mode::Read)) == 0)
    return std::unexpected{ResultCode::AccessDenied};

  const u32 file_size = static_cast<u32>(host_file->GetSize());
  // IOS has this check in the read request handler.
  if (count + handle->file_offset > file_size)
    count = file_size - handle->file_offset;

  // File might be opened twice, need to seek before we read
  host_file->Seek(handle->file_offset, File::SeekOrigin::Begin);

  // count stops at the end of the file: a short read is an error
  size_t actually_read = 0;
  host_file->ReadArray(ptr, count, &actually_read);
  host_file->ClearError();
  if (actually_read != count)
    return std::unexpected{ResultCode::AccessDenied};

  // IOS returns the number of bytes read and adds that value to the seek position,
  // instead of adding the *requested* read length.
  handle->file_offset += u32(actually_read);
  return u32(actually_read);
}

Result<u32> HostFileSystem::WriteBytesToFile(Fd fd, const u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  File::IOFile* const host_file = handle ? GetHostFile(handle) : nullptr;
  if (!host_file || !host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  if ((u8(handle->mode) & u8(Mode::Write)) == 0)
    return std::unexpected{ResultCode::AccessDenied};

  // File might be opened twice, need to seek before we read
  host_file->Seek(handle->file_offset, File::SeekOrigin::Begin);
  if (!host_file->WriteBytes(ptr, count))
    return std::unexpected{ResultCode::AccessDenied};

  handle->file_offset += count;
  return count;
}

Result<u32> HostFileSystem::SeekFile(Fd fd, std::uint32_t offset, SeekMode mode)
{
  Handle* handle = GetHandleFromFd(fd);
  File::IOFile* const host_file = handle ? GetHostFile(handle) : nullptr;
  if (!host_file || !host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  u32 new_position = 0;
  switch (mode)
  {
  case SeekMode::Set:
    new_position = offset;
    break;
  case SeekMode::Current:
    new_position = handle->file_offset + offset;
    break;
  case SeekMode::End:
    new_position = host_file->GetSize() + offset;
    break;
  default:
    return std::unexpected{ResultCode::Invalid};
  }

  // This differs from POSIX behaviour which allows seeking past the end of the file.
  if (host_file->GetSize() < new_position)
    return std::unexpected{ResultCode::Invalid};

  handle->file_offset = new_position;
  return handle->file_offset;
}

Result<FileStatus> HostFileSystem::GetFileStatus(Fd fd)
{
  const Handle* handle = GetHandleFromFd(fd);
  File::IOFile* const host_file = handle ? GetHostFile(handle) : nullptr;
  if (!host_file || !host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  FileStatus status;
  status.size = host_file->GetSize();
  status.offset = handle->file_offset;
  return status;
}

HostFileSystem::Handle* HostFileSystem::AssignFreeHandle()
{
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
  {
    Handle* const handle = &m_handles[i];
    if (handle->opened)
      continue;
    // a closed handle never holds a host file
    *handle = Handle{};
    handle->opened = true;
    return handle;
  }
  return nullptr;
}

HostFileSystem::Handle* HostFileSystem::GetHandleFromFd(Fd fd)
{
  if (fd >= NUM_HANDLES || !m_handles[fd].opened)
    return nullptr;
  return &m_handles[fd];
}

Fd HostFileSystem::ConvertHandleToFd(const Handle* handle) const
{
  return Fd(handle - m_handles);
}

}  // namespace IOS::HLE::FS
