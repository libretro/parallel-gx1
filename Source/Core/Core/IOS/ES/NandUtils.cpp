// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/ES/ES.h"

#include <cinttypes>
#include <cstring>
#include <string>

#include <cstdio>

#include <lrc_hash.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE
{
static ES::TMDReader FindTMD(FSCore& fs, const char* tmd_path, Ticks ticks)
{
  const auto fd = fs.Open(PID_KERNEL, PID_KERNEL, tmd_path, FS::Mode::Read, ticks);
  if (fd.Get() < 0)
    return {};

  // read straight into the reader
  const u32 size = fs.GetFileStatus(fd.Get(), ticks)->size;
  ES::TMDReader tmd;
  u8* const bytes = tmd.ResizeBytes(size);
  if (size && (!bytes || !fs.Read(fd.Get(), bytes, size, ticks)))
    return {};

  return tmd;
}

ES::TMDReader ESCore::FindImportTMD(u64 title_id, Ticks ticks) const
{
  char path[Common::NAND_PATH_SIZE];
  std::snprintf(path, sizeof(path), "/import/%08x/%08x/content/title.tmd",
                static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
  return FindTMD(m_ios.GetFSCore(), path, ticks);
}

ES::TMDReader ESCore::FindInstalledTMD(u64 title_id, Ticks ticks) const
{
  char path[Common::NAND_PATH_SIZE];
  Common::GetTMDFileName(title_id, path, sizeof(path));
  return FindTMD(m_ios.GetFSCore(), path, ticks);
}

ES::TicketReader ESCore::FindSignedTicket(u64 title_id, int desired_version) const
{
  char path[Common::NAND_PATH_SIZE];
  if (desired_version == 1)
    Common::GetV1TicketFileName(title_id, path, sizeof(path));
  else
    Common::GetTicketFileName(title_id, path, sizeof(path));
  auto ticket_file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read);
  if (!ticket_file)
  {
    if (desired_version != ANY_TICKET_VERSION)
      // Desired ticket does not exist.
      return {};

    // Check if we are dealing with a v1 ticket
    Common::GetV1TicketFileName(title_id, path, sizeof(path));
    ticket_file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read);

    if (!ticket_file)
      return {};
  }

  // read straight into the reader
  const u32 size = ticket_file->GetStatus()->size;
  ES::TicketReader ticket;
  u8* const bytes = ticket.ResizeBytes(size);
  if (size && (!bytes || !ticket_file->Read(bytes, size)))
    return {};

  return ticket;
}

static bool IsValidPartOfTitleID(const char* string)
{
  size_t i;
  for (i = 0; i < 8; i++)
  {
    if (!Common::IsXDigit(string[i]))
      return false;
  }
  return string[8] == '\0';
}

static bool IsDirectory(FS::FileSystem* fs, const char* path)
{
  FS::Metadata metadata;
  return fs->GetMetadata(PID_KERNEL, PID_KERNEL, path, &metadata) == FS::ResultCode::Success &&
         !metadata.is_file;
}

static void ForEachTitleInTitleOrImport(FS::FileSystem* fs, const std::string& titles_dir,
                                        ESCore::TitleVisitor visit, void* context)
{
  FS::DirectoryList entries;
  if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, titles_dir.c_str(), &entries) !=
      FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "%s is not a directory", titles_dir.c_str());
    return;
  }

  // The /title and /import directories contain one directory per title type, and each of them has
  // a directory per title (where the name is the low 32 bits of the title ID in %08x format).
  bool keep_going = true;
  for (u32 i = 0; keep_going && i < entries.count; i++)
  {
    const char* const title_type = entries.names[i];
    if (!IsValidPartOfTitleID(title_type))
      continue;

    const std::string title_dir = StringFromFormat("%s/%s", titles_dir.c_str(), title_type);
    FS::DirectoryList title_entries;
    if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, title_dir.c_str(), &title_entries) !=
        FS::ResultCode::Success)
    {
      continue;
    }

    for (u32 j = 0; keep_going && j < title_entries.count; j++)
    {
      const char* const title_identifier = title_entries.names[j];
      if (!IsValidPartOfTitleID(title_identifier))
        continue;
      if (!IsDirectory(fs, StringFromFormat("%s/%s", title_dir.c_str(), title_identifier).c_str()))
        continue;

      const u32 type = std::strtoul(title_type, nullptr, 16);
      const u32 identifier = std::strtoul(title_identifier, nullptr, 16);
      keep_going = visit(static_cast<u64>(type) << 32 | identifier, context);
    }
    FS::FreeDirectoryList(&title_entries);
  }
  FS::FreeDirectoryList(&entries);
}

static bool CountTitle(u64 title_id, void* context)
{
  (void)title_id;
  ++*static_cast<u32*>(context);
  return true;
}

void ESCore::ForEachInstalledTitle(TitleVisitor visit, void* context) const
{
  ForEachTitleInTitleOrImport(m_ios.GetFS(), "/title", visit, context);
}

u32 ESCore::GetInstalledTitleCount() const
{
  u32 count = 0;
  ForEachInstalledTitle(CountTitle, &count);
  return count;
}

void ESCore::ForEachTitleImport(TitleVisitor visit, void* context) const
{
  ForEachTitleInTitleOrImport(m_ios.GetFS(), "/import", visit, context);
}

u32 ESCore::GetTitleWithTicketCount() const
{
  u32 count = 0;
  ForEachTitleWithTicket(CountTitle, &count);
  return count;
}

void ESCore::ForEachTitleWithTicket(TitleVisitor visit, void* context) const
{
  const auto fs = m_ios.GetFS();
  FS::DirectoryList entries;
  if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/ticket", &entries) != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "/ticket is not a directory");
    return;
  }

  // The /ticket directory contains one directory per title type, and each of them contains
  // one ticket per title (where the name is the low 32 bits of the title ID in %08x format).
  bool keep_going = true;
  for (u32 i = 0; keep_going && i < entries.count; i++)
  {
    const char* const title_type = entries.names[i];
    if (!IsValidPartOfTitleID(title_type))
      continue;

    char sub_dir[sizeof("/ticket/") + FS::MaxFilenameLength];
    std::snprintf(sub_dir, sizeof(sub_dir), "/ticket/%s", title_type);
    FS::DirectoryList sub_entries;
    if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, sub_dir, &sub_entries) != FS::ResultCode::Success)
    {
      continue;
    }

    for (u32 j = 0; keep_going && j < sub_entries.count; j++)
    {
      // Tickets are named after the low 32 bits of the title ID with a .tik or .tv1 extension.
      const char* const file_name = sub_entries.names[j];
      char name_without_ext[9];
      char path[FS::MaxPathLength + 1];
      const size_t prefix_length = strnlen(file_name, 8);
      std::memcpy(name_without_ext, file_name, prefix_length);
      name_without_ext[prefix_length] = '\0';
      std::snprintf(path, sizeof(path), "%s/%s", sub_dir, file_name);
      if (IsDirectory(fs, path) || !IsValidPartOfTitleID(name_without_ext) ||
          std::strlen(file_name) != 12 ||
          (std::strcmp(file_name + 8, ".tik") && std::strcmp(file_name + 8, ".tv1")))
      {
        continue;
      }

      const u32 type = std::strtoul(title_type, nullptr, 16);
      const u32 identifier = std::strtoul(name_without_ext, nullptr, 16);
      keep_going = visit(static_cast<u64>(type) << 32 | identifier, context);
    }
    FS::FreeDirectoryList(&sub_entries);
  }
  FS::FreeDirectoryList(&entries);
}

u16 ESCore::GetStoredContentsCount(const ES::TMDReader& tmd,
                                   CheckContentHashes check_content_hashes) const
{
  if (!tmd.IsValid())
    return 0;
  u16 count = 0;
  for (u16 i = 0; i < tmd.GetNumContents(); ++i)
  {
    ES::Content content;
    if (tmd.GetContent(i, &content) && IsContentStored(tmd, content, check_content_hashes))
      ++count;
  }
  return count;
}

bool ESCore::IsContentStored(const ES::TMDReader& tmd, const ES::Content& content,
                             CheckContentHashes check_content_hashes) const
{
  const auto fs = m_ios.GetFS();
  char path[64];
  if (!GetContentPath(tmd.GetTitleId(), content, path, sizeof(path)))
    return false;

  // Check whether the content file exists.
  const auto file = fs->OpenFile(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read);
  if (!file.has_value())
    return false;

  // If content hash checks are disabled, all we have to do is check for existence.
  if (check_content_hashes == CheckContentHashes::No)
    return true;

  // Otherwise, check whether the installed content SHA1 matches the expected hash, hashing
  // the content in chunks rather than reading it whole.
  struct sha1_state sha1;
  u8 digest[20];
  u8 chunk[2048];
  sha1_stream_init(&sha1);
  u32 left = file->GetStatus()->size;
  bool ok = true;
  while (left)
  {
    const u32 n = left < sizeof(chunk) ? left : u32(sizeof(chunk));
    if (!file->Read(chunk, n))
    {
      ok = false;
      break;
    }
    sha1_stream_update(&sha1, chunk, n);
    left -= n;
  }
  return ok && sha1_stream_final(&sha1, digest) &&
         !std::memcmp(digest, content.sha1, sizeof(digest));
}

u32 ESCore::GetSharedContentsCount() const
{
  FS::FileSystem* const fs = m_ios.GetFS();
  FS::DirectoryList entries;
  u32 count = 0;
  fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/shared1", &entries);
  for (u32 i = 0; i < entries.count; i++)
  {
    const char* const entry = entries.names[i];
    char path[FS::MaxPathLength + 1];
    std::snprintf(path, sizeof(path), "/shared1/%s", entry);
    if (!IsDirectory(fs, path) && std::strlen(entry) == 12 && !std::strcmp(entry + 8, ".app"))
      ++count;
  }
  FS::FreeDirectoryList(&entries);
  return count;
}

void ESCore::ForEachSharedContent(HashVisitor visit, void* context) const
{
  const ES::SharedContentMap map{m_ios.GetFSCore()};
  for (size_t i = 0; i < map.GetEntryCount(); i++)
  {
    if (!visit(map.GetHash(i), context))
      return;
  }
}

// Deletes path if it is empty, then each of its parents in turn up to the top-level directory.
static bool DeleteDirectoriesIfEmpty(FS::FileSystem* fs, const char* path)
{
  char directory_path[Common::NAND_PATH_SIZE];
  std::snprintf(directory_path, sizeof(directory_path), "%s", path);
  for (;;)
  {
    char* separator;
    FS::DirectoryList directory;
    const FS::ResultCode result =
        fs->ReadDirectory(PID_KERNEL, PID_KERNEL, directory_path, &directory);
    const bool is_empty = directory.count == 0;
    FS::FreeDirectoryList(&directory);
    if ((result == FS::ResultCode::Success && is_empty) ||
        (result != FS::ResultCode::Success && result != FS::ResultCode::NotFound))
    {
      if (fs->Delete(PID_KERNEL, PID_KERNEL, directory_path) != FS::ResultCode::Success)
        return false;
    }
    separator = std::strrchr(directory_path, '/');
    if (!separator || separator == directory_path)
      return true;
    *separator = '\0';
  }
}

constexpr FS::Modes title_dir_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::Read};
constexpr FS::Modes content_dir_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};
constexpr FS::Modes data_dir_modes{FS::Mode::ReadWrite, FS::Mode::None, FS::Mode::None};

bool ESCore::CreateTitleDirectories(u64 title_id, u16 group_id) const
{
  const auto fs = m_ios.GetFS();

  char content_dir[Common::NAND_PATH_SIZE];
  char content_dir_slash[Common::NAND_PATH_SIZE + 1];
  Common::GetTitleContentPath(title_id, content_dir, sizeof(content_dir));
  std::snprintf(content_dir_slash, sizeof(content_dir_slash), "%s/", content_dir);
  const auto result1 =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, content_dir_slash, 0, title_dir_modes);
  const auto result2 =
      fs->SetMetadata(PID_KERNEL, content_dir, PID_KERNEL, PID_KERNEL, 0, content_dir_modes);
  if (result1 != FS::ResultCode::Success || result2 != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to create or set metadata on content dir for %016" PRIx64,
              (u64)title_id);
    return false;
  }

  char data_dir[Common::NAND_PATH_SIZE];
  Common::GetTitleDataPath(title_id, data_dir, sizeof(data_dir));
  FS::Metadata data_dir_metadata;
  const FS::ResultCode data_dir_result =
      fs->GetMetadata(PID_KERNEL, PID_KERNEL, data_dir, &data_dir_metadata);
  if (data_dir_result == FS::ResultCode::Success ?
          data_dir_metadata.is_file :
          data_dir_result != FS::ResultCode::NotFound ||
              fs->CreateDirectory(PID_KERNEL, PID_KERNEL, data_dir, 0, data_dir_modes) !=
                  FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to create data dir for %016" PRIx64, (u64)title_id);
    return false;
  }

  ES::UIDSys uid_sys{m_ios.GetFSCore()};
  const u32 uid = uid_sys.GetOrInsertUIDForTitle(title_id);
  if (fs->SetMetadata(0, data_dir, uid, group_id, 0, data_dir_modes) != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to set metadata on data dir for %016" PRIx64, (u64)title_id);
    return false;
  }

  return true;
}

bool ESCore::InitImport(const ES::TMDReader& tmd)
{
  if (!CreateTitleDirectories(tmd.GetTitleId(), tmd.GetGroupId()))
    return false;

  const auto fs = m_ios.GetFS();
  const u64 title_id = tmd.GetTitleId();
  char import_content_dir[Common::NAND_PATH_SIZE];
  char import_content_dir_slash[Common::NAND_PATH_SIZE + 1];
  std::snprintf(import_content_dir, sizeof(import_content_dir), "/import/%08x/%08x/content",
                static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
  std::snprintf(import_content_dir_slash, sizeof(import_content_dir_slash), "%s/",
                import_content_dir);
  const auto result =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, import_content_dir_slash, 0, content_dir_modes);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "InitImport: Failed to create content dir for %016" PRIx64,
              (u64)tmd.GetTitleId());
    return false;
  }

  // IOS moves the title content directory to /import if the TMD exists during an import.
  char tmd_path[Common::NAND_PATH_SIZE];
  Common::GetTMDFileName(title_id, tmd_path, sizeof(tmd_path));
  FS::Metadata file_info;
  if (fs->GetMetadata(PID_KERNEL, PID_KERNEL, tmd_path, &file_info) != FS::ResultCode::Success ||
      !file_info.is_file)
  {
    return true;
  }

  char content_dir[Common::NAND_PATH_SIZE];
  Common::GetTitleContentPath(title_id, content_dir, sizeof(content_dir));
  const auto rename_result = fs->Rename(PID_KERNEL, PID_KERNEL, content_dir, import_content_dir);
  if (rename_result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "InitImport: Failed to move content dir for %016" PRIx64,
              (u64)tmd.GetTitleId());
    return false;
  }
  DeleteDirectoriesIfEmpty(m_ios.GetFS(), import_content_dir);
  return true;
}

// Whether an import's content directory keeps name: the TMD or one of its contents
static bool IsExpectedImportEntry(const char* name, const ES::TMDReader& tmd)
{
  if (!std::strcmp(name, "title.tmd"))
    return true;
  for (u16 i = 0; i < tmd.GetNumContents(); ++i)
  {
    ES::Content content;
    if (!tmd.GetContent(i, &content))
      continue;
    char content_name[16];
    std::snprintf(content_name, sizeof(content_name), "%08x.app", content.id);
    if (!std::strcmp(name, content_name))
      return true;
  }
  return false;
}

bool ESCore::FinishImport(const ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const u64 title_id = tmd.GetTitleId();
  char import_content_dir[sizeof("/import/00000000/00000000/content")];
  std::snprintf(import_content_dir, sizeof(import_content_dir), "/import/%08x/%08x/content",
                static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));

  // Remove everything not listed in the TMD.
  FS::DirectoryList entries;
  if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, import_content_dir, &entries) !=
      FS::ResultCode::Success)
  {
    return false;
  }
  for (u32 i = 0; i < entries.count; i++)
  {
    const char* const name = entries.names[i];
    char absolute_path[Common::NAND_PATH_SIZE];
    std::snprintf(absolute_path, sizeof(absolute_path), "%s/%s", import_content_dir, name);
    // There should not be any directory in there. Remove it.
    if (IsDirectory(fs, absolute_path))
      fs->Delete(PID_KERNEL, PID_KERNEL, absolute_path);
    else if (!IsExpectedImportEntry(name, tmd))
      fs->Delete(PID_KERNEL, PID_KERNEL, absolute_path);
  }
  FS::FreeDirectoryList(&entries);

  char content_dir[Common::NAND_PATH_SIZE];
  Common::GetTitleContentPath(title_id, content_dir, sizeof(content_dir));
  if (fs->Rename(PID_KERNEL, PID_KERNEL, import_content_dir, content_dir) !=
      FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "FinishImport: Failed to rename import directory to %s", content_dir);
    return false;
  }
  return true;
}

bool ESCore::WriteImportTMD(const ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const char* const tmd_path = "/tmp/title.tmd";
  {
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, tmd_path, content_dir_modes);
    size_t tmd_size;
    const u8* const tmd_bytes = tmd.GetBytes(&tmd_size);
    if (!file || !file->Write(tmd_bytes, tmd_size))
      return false;
  }

  const u64 title_id = tmd.GetTitleId();
  char dest[Common::NAND_PATH_SIZE];
  std::snprintf(dest, sizeof(dest), "/import/%08x/%08x/content/title.tmd",
                static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
  return fs->Rename(PID_KERNEL, PID_KERNEL, tmd_path, dest) == FS::ResultCode::Success;
}

void ESCore::FinishStaleImport(u64 title_id)
{
  const auto fs = m_ios.GetFS();
  const auto import_tmd = FindImportTMD(title_id);
  if (!import_tmd.IsValid())
  {
    char path[Common::NAND_PATH_SIZE];
    char import_content_dir[sizeof("/import/00000000/00000000/content")];
    Common::GetImportTitlePath(title_id, path, sizeof(path));
    std::snprintf(import_content_dir, sizeof(import_content_dir), "/import/%08x/%08x/content",
                  static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
    fs->Delete(PID_KERNEL, PID_KERNEL, import_content_dir);
    DeleteDirectoriesIfEmpty(fs, path);
    Common::GetTitlePath(title_id, path, sizeof(path));
    DeleteDirectoriesIfEmpty(fs, path);
  }
  else
  {
    FinishImport(import_tmd);
  }
}

bool ESCore::FinishStaleImportVisitor(u64 title_id, void* context)
{
  static_cast<ESCore*>(context)->FinishStaleImport(title_id);
  return true;
}

void ESCore::FinishAllStaleImports()
{
  ForEachTitleImport(FinishStaleImportVisitor, this);
}

bool ESCore::GetContentPath(const u64 title_id, const ES::Content& content, char* out,
                            size_t out_size, Ticks ticks) const
{
  if (content.IsShared())
  {
    u64 map_ticks = 0;
    const bool found = ES::SharedContentMap::FindFilename(m_ios.GetFSCore(), content.sha1, out,
                                                          out_size, &map_ticks);
    ticks.Add(map_ticks);
    return found;
  }
  // Common::GetTitleContentPath, without a root.
  const int length =
      snprintf(out, out_size, "/title/%08x/%08x/content/%08x.app", static_cast<u32>(title_id >> 32),
               static_cast<u32>(title_id), content.id);
  return length >= 0 && static_cast<size_t>(length) < out_size;
}

s32 ESDevice::WriteSystemFile(const char* path, const u8* data, u32 size, Ticks ticks)
{
  auto& fs = GetEmulationKernel().GetFSCore();
  const char* const last_separator = std::strrchr(path, '/');
  char tmp_path[sizeof("/tmp/") + Common::NAND_PATH_SIZE];
  std::snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", last_separator ? last_separator + 1 : path);

  auto result = fs.CreateFile(PID_KERNEL, PID_KERNEL, tmp_path, {},
                              {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None}, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to create temporary file %s: %d", tmp_path, static_cast<int>(result));
    return FS::ConvertResult(result);
  }

  auto fd = fs.Open(PID_KERNEL, PID_KERNEL, tmp_path, FS::Mode::ReadWrite, ticks);
  if (fd.Get() < 0)
  {
    ERROR_LOG(IOS_ES, "Failed to open temporary file %s: %" PRId64, tmp_path, (s64)fd.Get());
    return fd.Get();
  }

  if (fs.WriteBytes(fd.Get(), data, size, ticks) != s32(size))
  {
    ERROR_LOG(IOS_ES, "Failed to write to temporary file %s", tmp_path);
    return ES_EIO;
  }

  if (const auto ret = fs.Close(fd.Release(), ticks); ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "Failed to close temporary file %s", tmp_path);
    return ret;
  }

  result = fs.RenameFile(PID_KERNEL, PID_KERNEL, tmp_path, path, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to move launch file to final destination (%s): %d", path,
              static_cast<int>(result));
    return FS::ConvertResult(result);
  }

  return IPC_SUCCESS;
}

}  // namespace IOS::HLE
