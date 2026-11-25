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
static ES::TMDReader FindTMD(FSCore& fs, const std::string& tmd_path, Ticks ticks)
{
  const auto fd = fs.Open(PID_KERNEL, PID_KERNEL, tmd_path.c_str(), FS::Mode::Read, ticks);
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
  return FindTMD(m_ios.GetFSCore(), Common::GetImportTitlePath(title_id) + "/content/title.tmd",
                 ticks);
}

ES::TMDReader ESCore::FindInstalledTMD(u64 title_id, Ticks ticks) const
{
  return FindTMD(m_ios.GetFSCore(), Common::GetTMDFileName(title_id), ticks);
}

ES::TicketReader ESCore::FindSignedTicket(u64 title_id, int desired_version) const
{
  std::string path = desired_version == 1 ? Common::GetV1TicketFileName(title_id) :
                                            Common::GetTicketFileName(title_id);
  auto ticket_file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, path.c_str(), FS::Mode::Read);
  if (!ticket_file)
  {
    if (desired_version != ANY_TICKET_VERSION)
      // Desired ticket does not exist.
      return {};

    // Check if we are dealing with a v1 ticket
    path = Common::GetV1TicketFileName(title_id);
    ticket_file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, path.c_str(), FS::Mode::Read);

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

static bool IsValidPartOfTitleID(const std::string& string)
{
  if (string.length() != 8)
    return false;
  for (const char c : string)
  {
    if (!Common::IsXDigit(c))
      return false;
  }
  return true;
}

static void ForEachTitleInTitleOrImport(FS::FileSystem* fs, const std::string& titles_dir,
                                        ESCore::TitleVisitor visit, void* context)
{
  const auto entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, titles_dir);
  if (!entries)
  {
    ERROR_LOG(IOS_ES, "%s is not a directory", titles_dir.c_str());
    return;
  }

  // The /title and /import directories contain one directory per title type, and each of them has
  // a directory per title (where the name is the low 32 bits of the title ID in %08x format).
  for (const std::string& title_type : *entries)
  {
    if (!IsValidPartOfTitleID(title_type))
      continue;

    const std::string title_dir = StringFromFormat("%s/%s", titles_dir.c_str(), title_type.c_str());
    const auto title_entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, title_dir);
    if (!title_entries)
      continue;

    for (const std::string& title_identifier : *title_entries)
    {
      if (!IsValidPartOfTitleID(title_identifier))
        continue;
      if (!fs->ReadDirectory(
              PID_KERNEL, PID_KERNEL,
              StringFromFormat("%s/%s", title_dir.c_str(), title_identifier.c_str())))
      {
        continue;
      }

      const u32 type = std::stoul(title_type, nullptr, 16);
      const u32 identifier = std::stoul(title_identifier, nullptr, 16);
      if (!visit(static_cast<u64>(type) << 32 | identifier, context))
        return;
    }
  }
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
  const auto entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/ticket");
  if (!entries)
  {
    ERROR_LOG(IOS_ES, "/ticket is not a directory");
    return;
  }

  // The /ticket directory contains one directory per title type, and each of them contains
  // one ticket per title (where the name is the low 32 bits of the title ID in %08x format).
  for (const std::string& title_type : *entries)
  {
    if (!IsValidPartOfTitleID(title_type))
      continue;

    const auto sub_entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/ticket/" + title_type);
    if (!sub_entries)
      continue;

    for (const std::string& file_name : *sub_entries)
    {
      const std::string name_without_ext = file_name.substr(0, 8);
      if (fs->ReadDirectory(
              PID_KERNEL, PID_KERNEL,
              StringFromFormat("/ticket/%s/%s", title_type.c_str(), file_name.c_str())) ||
          !IsValidPartOfTitleID(name_without_ext) ||
          (name_without_ext + ".tik" != file_name && name_without_ext + ".tv1" != file_name))
      {
        continue;
      }

      const u32 type = std::stoul(title_type, nullptr, 16);
      const u32 identifier = std::stoul(name_without_ext, nullptr, 16);
      if (!visit(static_cast<u64>(type) << 32 | identifier, context))
        return;
    }
  }
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
  const auto entries = m_ios.GetFS()->ReadDirectory(PID_KERNEL, PID_KERNEL, "/shared1");
  u32 count = 0;
  for (const std::string& entry : *entries)
  {
    if (!m_ios.GetFS()->ReadDirectory(PID_KERNEL, PID_KERNEL, "/shared1/" + entry) &&
        entry.size() == 12 && entry.compare(8, 4, ".app") == 0)
    {
      ++count;
    }
  }
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

static bool DeleteDirectoriesIfEmpty(FS::FileSystem* fs, const std::string& path)
{
  std::string::size_type position = std::string::npos;
  do
  {
    const auto directory = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, path.substr(0, position));
    if ((directory && directory->empty()) ||
        (!directory && directory.error() != FS::ResultCode::NotFound))
    {
      if (fs->Delete(PID_KERNEL, PID_KERNEL, path.substr(0, position)) != FS::ResultCode::Success)
        return false;
    }
    position = path.find_last_of('/', position - 1);
  } while (position != 0);
  return true;
}

constexpr FS::Modes title_dir_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::Read};
constexpr FS::Modes content_dir_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};
constexpr FS::Modes data_dir_modes{FS::Mode::ReadWrite, FS::Mode::None, FS::Mode::None};

bool ESCore::CreateTitleDirectories(u64 title_id, u16 group_id) const
{
  const auto fs = m_ios.GetFS();

  const std::string content_dir = Common::GetTitleContentPath(title_id);
  const auto result1 =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, content_dir + '/', 0, title_dir_modes);
  const auto result2 =
      fs->SetMetadata(PID_KERNEL, content_dir, PID_KERNEL, PID_KERNEL, 0, content_dir_modes);
  if (result1 != FS::ResultCode::Success || result2 != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to create or set metadata on content dir for %016" PRIx64,
              (u64)title_id);
    return false;
  }

  const std::string data_dir = Common::GetTitleDataPath(title_id);
  const auto data_dir_contents = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, data_dir);
  if (!data_dir_contents && (data_dir_contents.error() != FS::ResultCode::NotFound ||
                             fs->CreateDirectory(PID_KERNEL, PID_KERNEL, data_dir, 0,
                                                 data_dir_modes) != FS::ResultCode::Success))
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
  const std::string import_content_dir = Common::GetImportTitlePath(tmd.GetTitleId()) + "/content";
  const auto result =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, import_content_dir + '/', 0, content_dir_modes);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "InitImport: Failed to create content dir for %016" PRIx64,
              (u64)tmd.GetTitleId());
    return false;
  }

  // IOS moves the title content directory to /import if the TMD exists during an import.
  const auto file_info =
      fs->GetMetadata(PID_KERNEL, PID_KERNEL, Common::GetTMDFileName(tmd.GetTitleId()));
  if (!file_info || !file_info->is_file)
    return true;

  const std::string content_dir = Common::GetTitleContentPath(tmd.GetTitleId());
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
  const std::string import_content_dir = Common::GetImportTitlePath(title_id) + "/content";

  // Remove everything not listed in the TMD.
  const auto entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, import_content_dir);
  if (!entries)
    return false;
  for (const std::string& name : *entries)
  {
    const std::string absolute_path =
        StringFromFormat("%s/%s", import_content_dir.c_str(), name.c_str());
    // There should not be any directory in there. Remove it.
    if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, absolute_path))
      fs->Delete(PID_KERNEL, PID_KERNEL, absolute_path);
    else if (!IsExpectedImportEntry(name.c_str(), tmd))
      fs->Delete(PID_KERNEL, PID_KERNEL, absolute_path);
  }

  const std::string content_dir = Common::GetTitleContentPath(title_id);
  if (fs->Rename(PID_KERNEL, PID_KERNEL, import_content_dir, content_dir) !=
      FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "FinishImport: Failed to rename import directory to %s", content_dir.c_str());
    return false;
  }
  return true;
}

bool ESCore::WriteImportTMD(const ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const std::string tmd_path = "/tmp/title.tmd";
  {
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, tmd_path, content_dir_modes);
    size_t tmd_size;
    const u8* const tmd_bytes = tmd.GetBytes(&tmd_size);
    if (!file || !file->Write(tmd_bytes, tmd_size))
      return false;
  }

  const std::string dest = StringFromFormat("%s/content/title.tmd",
                                            Common::GetImportTitlePath(tmd.GetTitleId()).c_str());
  return fs->Rename(PID_KERNEL, PID_KERNEL, tmd_path, dest) == FS::ResultCode::Success;
}

void ESCore::FinishStaleImport(u64 title_id)
{
  const auto fs = m_ios.GetFS();
  const auto import_tmd = FindImportTMD(title_id);
  if (!import_tmd.IsValid())
  {
    fs->Delete(PID_KERNEL, PID_KERNEL, Common::GetImportTitlePath(title_id) + "/content");
    DeleteDirectoriesIfEmpty(fs, Common::GetImportTitlePath(title_id));
    DeleteDirectoriesIfEmpty(fs, Common::GetTitlePath(title_id));
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

std::string ESCore::GetContentPath(const u64 title_id, const ES::Content& content,
                                   Ticks ticks) const
{
  if (content.IsShared())
  {
    ES::SharedContentMap content_map{m_ios.GetFSCore()};
    ticks.Add(content_map.GetTicks());
    std::string filename;
    content_map.GetFilenameFromSHA1(content.sha1, &filename);
    return filename;
  }
  return StringFromFormat("%s/%08x.app", Common::GetTitleContentPath(title_id).c_str(), content.id);
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

s32 ESDevice::WriteSystemFile(const std::string& path, const u8* data, u32 size, Ticks ticks)
{
  auto& fs = GetEmulationKernel().GetFSCore();
  const std::string tmp_path = "/tmp/" + PathToFileName(path);

  auto result = fs.CreateFile(PID_KERNEL, PID_KERNEL, tmp_path, {},
                              {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None}, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to create temporary file %s: %d", tmp_path.c_str(),
              static_cast<int>(result));
    return FS::ConvertResult(result);
  }

  auto fd = fs.Open(PID_KERNEL, PID_KERNEL, tmp_path.c_str(), FS::Mode::ReadWrite, ticks);
  if (fd.Get() < 0)
  {
    ERROR_LOG(IOS_ES, "Failed to open temporary file %s: %" PRId64, tmp_path.c_str(),
              (s64)fd.Get());
    return fd.Get();
  }

  if (fs.WriteBytes(fd.Get(), data, size, ticks) != s32(size))
  {
    ERROR_LOG(IOS_ES, "Failed to write to temporary file %s", tmp_path.c_str());
    return ES_EIO;
  }

  if (const auto ret = fs.Close(fd.Release(), ticks); ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "Failed to close temporary file %s", tmp_path.c_str());
    return ret;
  }

  result = fs.RenameFile(PID_KERNEL, PID_KERNEL, tmp_path, path, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "Failed to move launch file to final destination (%s): %d", path.c_str(),
              static_cast<int>(result));
    return FS::ConvertResult(result);
  }

  return IPC_SUCCESS;
}

}  // namespace IOS::HLE
