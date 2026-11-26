// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/FileSystem.h"

#include <retro_dirent.h>
#include <retro_miscellaneous.h>

#include <cstdlib>
#include <cstring>
#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

#include <cstdio>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

namespace IOS::HLE::FS
{
bool FileSystem::BuildFilename(const char* wii_path, char* out, size_t out_size) const
{
  ASSERT(!m_root_path.empty());

  const size_t root_length = m_root_path.size();
  if (root_length >= out_size)
    return false;
  std::memcpy(out, m_root_path.c_str(), root_length + 1);
  if (wii_path[0] == '/')
    return Common::EscapePathTo(wii_path, out + root_length, out_size - root_length);

  ASSERT_MSG(IOS_FS, false, "Invalid Wii path '%s' given to BuildFilename()", wii_path);
  return true;
}

namespace
{
struct SerializedFstEntry
{
  std::string_view GetName() const { return {name, strnlen(name, ARRAY_SIZE(name))}; }
  void SetName(std::string_view new_name)
  {
    const size_t size = new_name.length() < ARRAY_SIZE(name) ? new_name.length() : ARRAY_SIZE(name);
    std::memcpy(name, new_name.data(), size);
  }

  /// File name
  char name[12]{};
  /// File owner user ID
  Common::BigEndianValue<Uid> uid{};
  /// File owner group ID
  Common::BigEndianValue<Gid> gid{};
  /// Is this a file or a directory?
  bool is_file = false;
  /// File access modes
  Modes modes{};
  /// File attribute
  FileAttribute attribute{};
  /// Unknown property
  Common::BigEndianValue<u32> x3{};
  /// Number of children
  Common::BigEndianValue<u32> num_children{};
};
static_assert(std::is_standard_layout<SerializedFstEntry>());
static_assert(sizeof(SerializedFstEntry) == 0x20);

template <typename D, typename S>
static void CopyMetadataFields(D* dst, const S& src)
{
  dst->uid = src.uid;
  dst->gid = src.gid;
  dst->is_file = src.is_file;
  dst->modes = src.modes;
  dst->attribute = src.attribute;
}

// Convert the host directory entries into ones that can be exposed to the emulated system.
static u64 FixupDirectoryEntries(File::FSTEntry* dir, bool is_root)
{
  u64 removed_entries = 0;
  for (auto it = dir->children.begin(); it != dir->children.end();)
  {
    // Drop files in the root of the Wii NAND folder, since we store extra files there that the
    // emulated system shouldn't know about.
    if (is_root && !it->isDirectory)
    {
      ++removed_entries;
      it = dir->children.erase(it);
      continue;
    }

    // Decode escaped invalid file system characters so that games (such as Harry Potter and the
    // Half-Blood Prince) can find what they expect.
    it->virtualName.resize(Common::UnescapeFileName(&it->virtualName[0], it->virtualName.size()));

    // Drop files that have too long filenames.
    if (!IsValidFilename(it->virtualName))
    {
      if (it->isDirectory)
        removed_entries += it->size;
      ++removed_entries;
      it = dir->children.erase(it);
      continue;
    }

    if (dir->isDirectory)
      removed_entries += FixupDirectoryEntries(&*it, false);

    ++it;
  }
  dir->size -= removed_entries;
  return removed_entries;
}
}  // namespace

bool FileSystem::FstEntry::CheckPermission(Uid caller_uid, Gid caller_gid,
                                           Mode requested_mode) const
{
  if (caller_uid == 0)
    return true;
  Mode file_mode = data.modes.other;
  if (data.uid == caller_uid)
    file_mode = data.modes.owner;
  else if (data.gid == caller_gid)
    file_mode = data.modes.group;
  return (u8(requested_mode) & u8(file_mode)) == u8(requested_mode);
}

void FileSystem::FreeFstChildren(FstEntry* entry)
{
  for (u32 i = 0; i < entry->child_count; i++)
    FreeFstChildren(&entry->children[i]);
  std::free(entry->children);
  entry->children = nullptr;
  entry->child_count = 0;
  entry->child_capacity = 0;
}

FileSystem::FstEntry* FileSystem::AddFstChild(FstEntry* parent)
{
  if (parent->child_count == parent->child_capacity)
  {
    const u32 capacity = parent->child_capacity ? parent->child_capacity * 2 : 4;
    FstEntry* const grown =
        static_cast<FstEntry*>(std::realloc(parent->children, capacity * sizeof(FstEntry)));
    if (!grown)
      return nullptr;
    parent->children = grown;
    parent->child_capacity = capacity;
  }
  FstEntry* const child = &parent->children[parent->child_count++];
  std::memset(child, 0, sizeof(*child));
  return child;
}

void FileSystem::RemoveFstChild(FstEntry* parent, u32 index)
{
  FreeFstChildren(&parent->children[index]);
  std::memmove(&parent->children[index], &parent->children[index + 1],
               (parent->child_count - index - 1) * sizeof(FstEntry));
  parent->child_count--;
}

FileSystem::FstEntry* FileSystem::FindFstChild(FstEntry* parent, const char* name)
{
  for (u32 i = 0; i < parent->child_count; i++)
  {
    if (!std::strcmp(parent->children[i].name, name))
      return &parent->children[i];
  }
  return nullptr;
}

// Copies name into an FST entry's, cut to the longest a valid path has
static void SetFstName(char* out, const char* name, size_t name_length)
{
  if (name_length > MaxPathLength)
    name_length = MaxPathLength;
  std::memcpy(out, name, name_length);
  out[name_length] = '\0';
}

FileSystem::FileSystem(std::string root_path) : m_root_path{std::move(root_path)}
{
  while (m_root_path.ends_with('/'))
    m_root_path.pop_back();
  File::CreateFullPath(m_root_path + '/');
  ResetFst();
  LoadFst();
}

FileSystem::~FileSystem()
{
  CloseAllHandles();
  FreeFstChildren(&m_root_entry);
}

std::string FileSystem::GetFstFilePath() const
{
  return StringFromFormat("%s/fst.bin", m_root_path.c_str());
}

void FileSystem::ResetFst()
{
  FreeFstChildren(&m_root_entry);
  std::memset(&m_root_entry, 0, sizeof(m_root_entry));
  SetFstName(m_root_entry.name, "/", 1);
  // Mode 0x16 (Directory | Owner_None | Group_Read | Other_Read) in the FS sysmodule
  m_root_entry.data.modes = {Mode::None, Mode::Read, Mode::Read};
}

void FileSystem::LoadFst()
{
  File::IOFile file{GetFstFilePath(), "rb"};
  // Existing filesystems will not have a FST. This is not a problem,
  // as the rest of FileSystem will use sane defaults.
  if (!file)
    return;

  // Each child is parsed in place in its parent's list, not copied up into it
  const auto parse_entry = [&file](const auto& parse, size_t depth, FstEntry* result) -> bool {
    if (depth > MaxPathDepth)
      return false;

    SerializedFstEntry entry;
    if (!file.ReadArray(&entry, 1))
      return false;

    SetFstName(result->name, entry.name, strnlen(entry.name, ARRAY_SIZE(entry.name)));
    CopyMetadataFields(&result->data, entry);
    for (size_t i = 0; i < entry.num_children; ++i)
    {
      // The child is parsed into place; the parent's array may move while it is
      FstEntry* const child = AddFstChild(result);
      if (!child || !parse(parse, depth + 1, child))
        return false;
    }
    return true;
  };

  FstEntry root_entry = {};
  if (!parse_entry(parse_entry, 0, &root_entry))
  {
    ERROR_LOG(IOS_FS, "Failed to parse FST: at least one of the entries was invalid");
    FreeFstChildren(&root_entry);
    return;
  }
  FreeFstChildren(&m_root_entry);
  m_root_entry = root_entry;
}

void FileSystem::SaveFst()
{
  // The tree, depth first, each entry followed by its children
  const auto count_entries = [](const auto& count, const FstEntry& entry) -> size_t {
    size_t total = 1;
    for (u32 i = 0; i < entry.child_count; i++)
      total += count(count, entry.children[i]);
    return total;
  };
  const size_t entry_count = count_entries(count_entries, m_root_entry);
  SerializedFstEntry* const to_write =
      static_cast<SerializedFstEntry*>(std::calloc(entry_count, sizeof(SerializedFstEntry)));
  if (!to_write)
  {
    PanicAlert("IOS_FS: Failed to write new FST");
    return;
  }
  size_t written = 0;
  const auto collect_entries = [to_write, &written](const auto& collect,
                                                    const FstEntry& entry) -> void {
    SerializedFstEntry& serialized = to_write[written++];
    serialized.SetName(entry.name);
    CopyMetadataFields(&serialized, entry.data);
    serialized.num_children = entry.child_count;
    for (u32 i = 0; i < entry.child_count; i++)
      collect(collect, entry.children[i]);
  };
  collect_entries(collect_entries, m_root_entry);

  const std::string dest_path = GetFstFilePath();
  const std::string temp_path = File::GetTempFilenameForAtomicWrite(dest_path);
  {
    // This temporary file must be closed before it can be renamed.
    File::IOFile file{temp_path, "wb"};
    const bool ok = file.WriteArray(to_write, entry_count);
    std::free(to_write);
    if (!ok)
    {
      PanicAlert("IOS_FS: Failed to write new FST");
      return;
    }
  }
  if (!File::Rename(temp_path, dest_path))
    PanicAlert("IOS_FS: Failed to rename temporary FST file");
}

FileSystem::FstEntry* FileSystem::GetFstEntryForPath(const char* path)
{
  if (!std::strcmp(path, "/"))
    return &m_root_entry;

  if (!IsValidNonRootPath(path))
    return nullptr;

  char host_path[HOST_PATH_SIZE];
  if (!BuildFilename(path, host_path, sizeof(host_path)))
    return nullptr;
  const File::FileInfo host_file_info{host_path};
  if (!host_file_info.Exists())
    return nullptr;

  // A valid non-root path is '/' and components of at most MaxFilenameLength between '/'s
  FstEntry* entry = &m_root_entry;
  const char* component = path + 1;
  while (*component)
  {
    const char* const end = std::strchr(component, '/');
    const size_t length = end ? size_t(end - component) : std::strlen(component);
    char name[MaxPathLength + 1];
    SetFstName(name, component, length);

    FstEntry* next = FindFstChild(entry, name);
    if (!next)
    {
      // Fall back to dummy data to avoid breaking existing filesystems.
      // This code path is also reached when creating a new file or directory;
      // proper metadata is filled in later.
      INFO_LOG(IOS_FS, "Creating a default entry for %.*s", int(component + length - path), path);
      next = AddFstChild(entry);
      if (!next)
        return nullptr;
      std::memcpy(next->name, name, sizeof(name));
      next->data.modes = {Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
    }
    entry = next;
    component += length;
    if (*component == '/')
      component++;
  }

  entry->data.is_file = host_file_info.IsFile();
  if (entry->data.is_file && entry->child_count)
  {
    WARN_LOG(IOS_FS, "%s is a file but also has children; clearing children", path);
    FreeFstChildren(entry);
  }

  return entry;
}

void FileSystem::DoState(PointerWrap& p)
{
  // This piece of code is handling four separate problems:
  // 1. Close host handles by calling reset on them, in case DoStateRead needs to modify a file that
  //    was open.
  // 2. Close guest handles by setting opened to false on each element in m_handles, in case
  //    DoStateRead needs to modify a file that was open.
  // 3. Close guest handles by setting opened to false on each element in m_handles, because if all
  //    of them were open, it would make DoStateRead/DoStateWriteOrMeasure's calls to OpenFile fail.
  // 4. Create a copy of m_handles that we can restore later in case we're writing/measuring,
  //    because OpenFile happily stomps over elements in m_handles that have opened set to false.
  Handle handles_copy[NUM_HANDLES];
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
  {
    handles_copy[i] = m_handles[i];
    handles_copy[i].host_file = -1;
  }
  CloseAllHandles();

  // The format for the next part of the save state is follows:
  // 1. bool is_full_nand_in_state (movie active && temporary wii root)
  // 2. Contents of the "/tmp" directory recursively.
  // 3. u32 size_of_nand (or 0, if not is_full_nand_in_state).
  // 4. Contents of the "/" directory recursively (or nothing, if the
  // root of the NAND folder is not save stated).

  // The "/" directory is only saved when a savestate is made during a movie recording
  // and when the directory root is temporary (i.e. WiiSession).
  // If a save state is made during a movie recording and is loaded when no movie is active,
  // then a call to p.DoExternal() will be used to skip over reading the contents of the "/"
  // directory (it skips over the number of bytes specified by size_of_nand)

  // No movie is ever recorded, so the full NAND is never wanted.
  const bool is_full_nand_wanted = false;

  bool is_full_nand_in_state = is_full_nand_wanted;
  p.Do(is_full_nand_in_state);

  if (!p.IsReadMode())
  {
    DoStateWriteOrMeasure(p, "/tmp");
    u8* const nand_size_ptr = p.ReserveU32();
    if (is_full_nand_in_state)
    {
      DoStateWriteOrMeasure(p, "/");
      if (p.IsWriteMode())
      {
        const u32 size_of_nand = p.GetOffsetFromPreviousPosition(nand_size_ptr) - sizeof(u32);
        memcpy(nand_size_ptr, &size_of_nand, sizeof(size_of_nand));
      }
    }

    for (i = 0; i < NUM_HANDLES; i++)
      m_handles[i] = std::move(handles_copy[i]);
  }
  else  // case where we're in read mode.
  {
    u32 size_of_nand = 0;
    DoStateRead(p, "/tmp");
    if (is_full_nand_in_state && is_full_nand_wanted)
    {
      p.Do(size_of_nand);
      DoStateRead(p, "/");
    }
    else
    {
      // Skip over any NAND data without using it.
      (void)p.DoExternal(size_of_nand);
    }
  }

  for (i = 0; i < NUM_HANDLES; i++)
  {
    Handle& handle = m_handles[i];
    p.Do(handle.opened);
    p.Do(handle.mode);
    // As a string was: its length, then its characters.
    u32 path_length = static_cast<u32>(std::strlen(handle.wii_path));
    p.Do(path_length);
    if (path_length >= sizeof(handle.wii_path))
    {
      p.SetMeasureMode();
      return;
    }
    p.DoArray(handle.wii_path, path_length);
    handle.wii_path[path_length] = '\0';
    p.Do(handle.file_offset);
    if (handle.opened)
    {
      char host_path[HOST_PATH_SIZE];
      handle.host_file = BuildFilename(handle.wii_path, host_path, sizeof(host_path)) ?
                             OpenHostFile(host_path) :
                             -1;
    }
  }
}

ResultCode FileSystem::Format(Uid uid)
{
  if (uid != 0)
    return ResultCode::AccessDenied;
  if (m_root_path.empty())
    return ResultCode::AccessDenied;
  char root[HOST_PATH_SIZE];
  if (!BuildFilename("/", root, sizeof(root)) || !File::DeleteDirRecursively(root) ||
      !File::CreateDir(root))
    return ResultCode::UnknownError;
  ResetFst();
  SaveFst();
  // Reset and close all handles.
  CloseAllHandles();
  return ResultCode::Success;
}

ResultCode FileSystem::CreateFileOrDirectory(Uid uid, Gid gid, const char* path, FileAttribute attr,
                                             Modes modes, bool is_file)
{
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;

  int depth = 0;
  for (const char* p = path; *p; p++)
  {
    const char c = *p;
    if (!Common::IsPrintableCharacter(c))
      return ResultCode::Invalid;
    if (c == '/')
      depth++;
  }

  if (!is_file && depth > int(MaxPathDepth))
    return ResultCode::TooManyPathComponents;

  SplitPathResult split_path;
  SplitPathAndBasename(path, &split_path);
  char host_path[HOST_PATH_SIZE];
  if (!BuildFilename(path, host_path, sizeof(host_path)))
    return ResultCode::Invalid;

  FstEntry* parent = GetFstEntryForPath(split_path.parent);
  if (!parent)
    return ResultCode::NotFound;

  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;

  if (File::Exists(host_path))
    return ResultCode::AlreadyExists;

  const bool ok = is_file ? File::CreateEmptyFile(host_path) : File::CreateDir(host_path);
  if (!ok)
  {
    ERROR_LOG(IOS_FS, "Failed to create file or directory: %s", host_path);
    return ResultCode::UnknownError;
  }

  FstEntry* child = GetFstEntryForPath(path);
  if (!child)
    return ResultCode::UnknownError;
  FreeFstChildren(child);
  std::memset(child, 0, sizeof(*child));
  SetFstName(child->name, split_path.file_name, std::strlen(split_path.file_name));
  child->data.is_file = is_file;
  child->data.modes = modes;
  child->data.uid = uid;
  child->data.gid = gid;
  child->data.attribute = attr;
  SaveFst();
  return ResultCode::Success;
}

ResultCode FileSystem::CreateFile(Uid uid, Gid gid, const char* path, FileAttribute attr,
                                  Modes modes)
{
  return CreateFileOrDirectory(uid, gid, path, attr, modes, true);
}

ResultCode FileSystem::CreateDirectory(Uid uid, Gid gid, const char* path, FileAttribute attr,
                                       Modes modes)
{
  return CreateFileOrDirectory(uid, gid, path, attr, modes, false);
}

bool FileSystem::IsFileOpened(const char* path) const
{
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
  {
    if (m_handles[i].opened && std::strcmp(m_handles[i].wii_path, path) == 0)
      return true;
  }
  return false;
}

bool FileSystem::IsDirectoryInUse(const char* path) const
{
  int i;
  for (i = 0; i < NUM_HANDLES; i++)
  {
    if (m_handles[i].opened && std::strncmp(m_handles[i].wii_path, path, std::strlen(path)) == 0)
      return true;
  }
  return false;
}

ResultCode FileSystem::Delete(Uid uid, Gid gid, const char* path)
{
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;

  char host_path[HOST_PATH_SIZE];
  if (!BuildFilename(path, host_path, sizeof(host_path)))
    return ResultCode::Invalid;
  SplitPathResult split_path;
  SplitPathAndBasename(path, &split_path);

  FstEntry* parent = GetFstEntryForPath(split_path.parent);
  if (!parent)
    return ResultCode::NotFound;

  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;

  if (!File::Exists(host_path))
    return ResultCode::NotFound;

  if (File::IsFile(host_path) && !IsFileOpened(path))
    File::Delete(host_path);
  else if (File::IsDirectory(host_path) && !IsDirectoryInUse(path))
    File::DeleteDirRecursively(host_path);
  else
    return ResultCode::InUse;

  for (u32 i = 0; i < parent->child_count; i++)
  {
    if (!std::strcmp(parent->children[i].name, split_path.file_name))
    {
      RemoveFstChild(parent, i);
      break;
    }
  }
  SaveFst();

  return ResultCode::Success;
}

// Moves the host file or directory, replacing one of the same type at the new path
static ResultCode RenameHostPath(const char* host_old_path, const char* host_new_path)
{
  // If there is already something of the same type at the new path, delete it.
  if (File::Exists(host_new_path))
  {
    const bool old_is_file = File::IsFile(host_old_path);
    const bool new_is_file = File::IsFile(host_new_path);
    if (old_is_file && new_is_file)
      File::Delete(host_new_path);
    else if (!old_is_file && !new_is_file)
      File::DeleteDirRecursively(host_new_path);
    else
      return ResultCode::Invalid;
  }

  if (!File::Rename(host_old_path, host_new_path))
  {
    ERROR_LOG(IOS_FS, "Rename %s to %s - failed", host_old_path, host_new_path);
    return ResultCode::NotFound;
  }
  return ResultCode::Success;
}

ResultCode FileSystem::Rename(Uid uid, Gid gid, const char* old_path, const char* new_path)
{
  if (!IsValidNonRootPath(old_path) || !IsValidNonRootPath(new_path))
    return ResultCode::Invalid;

  SplitPathResult split_old_path;
  SplitPathAndBasename(old_path, &split_old_path);
  SplitPathResult split_new_path;
  SplitPathAndBasename(new_path, &split_new_path);

  FstEntry* old_parent = GetFstEntryForPath(split_old_path.parent);
  FstEntry* new_parent = GetFstEntryForPath(split_new_path.parent);
  if (!old_parent || !new_parent)
    return ResultCode::NotFound;

  if (!old_parent->CheckPermission(uid, gid, Mode::Write) ||
      !new_parent->CheckPermission(uid, gid, Mode::Write))
  {
    return ResultCode::AccessDenied;
  }

  FstEntry* entry = GetFstEntryForPath(old_path);
  if (!entry)
    return ResultCode::NotFound;

  // For files, the file name is not allowed to change.
  if (entry->data.is_file && std::strcmp(split_old_path.file_name, split_new_path.file_name))
    return ResultCode::Invalid;

  if ((!entry->data.is_file && IsDirectoryInUse(old_path)) ||
      (entry->data.is_file && IsFileOpened(old_path)))
  {
    return ResultCode::InUse;
  }

  // Both host paths together are too large for the stack
  char* const host_old_path = static_cast<char*>(std::malloc(2 * HOST_PATH_SIZE));
  if (!host_old_path)
    return ResultCode::UnknownError;
  char* const host_new_path = host_old_path + HOST_PATH_SIZE;
  if (!BuildFilename(old_path, host_old_path, HOST_PATH_SIZE) ||
      !BuildFilename(new_path, host_new_path, HOST_PATH_SIZE))
  {
    std::free(host_old_path);
    return ResultCode::Invalid;
  }
  const ResultCode moved = RenameHostPath(host_old_path, host_new_path);
  std::free(host_old_path);
  if (moved != ResultCode::Success)
    return moved;

  FstEntry* new_entry = GetFstEntryForPath(new_path);
  if (!new_entry)
  {
    SaveFst();
    return ResultCode::Success;
  }
  SetFstName(new_entry->name, split_new_path.file_name, std::strlen(split_new_path.file_name));
  // Adding the new entry may have moved the array the old parent lives in
  old_parent = GetFstEntryForPath(split_old_path.parent);

  // Finally, remove the child from the old parent and move it to the new parent.
  for (u32 i = 0; old_parent && i < old_parent->child_count; i++)
  {
    FstEntry& old_entry = old_parent->children[i];
    if (std::strcmp(old_entry.name, split_old_path.file_name))
      continue;
    if (&old_entry == new_entry)
      break;
    new_entry->data = old_entry.data;
    FreeFstChildren(new_entry);
    new_entry->children = old_entry.children;
    new_entry->child_count = old_entry.child_count;
    new_entry->child_capacity = old_entry.child_capacity;
    old_entry.children = nullptr;
    old_entry.child_count = 0;
    old_entry.child_capacity = 0;
    RemoveFstChild(old_parent, i);
    break;
  }

  SaveFst();

  return ResultCode::Success;
}

// Adds a host directory entry under the name the emulated system sees, if it can see it.
static bool AddDirectoryEntry(DirectoryList* list, u32* capacity, const char* host_name)
{
  // Each escape sequence takes six characters for one, so a longer name cannot unescape to a
  // valid one.
  char name[MaxFilenameLength * 6 + 1];
  size_t length = std::strlen(host_name);
  if (length >= sizeof(name))
    return true;
  std::memcpy(name, host_name, length + 1);

  // Decode escaped invalid file system characters so that games (such as Harry Potter and the
  // Half-Blood Prince) can find what they expect, and drop files that have too long filenames.
  length = Common::UnescapeFileName(name, length);
  if (length > MaxFilenameLength || std::memchr(name, '/', length))
    return true;

  if (list->count == *capacity)
  {
    const u32 new_capacity = *capacity ? *capacity * 2 : 16;
    void* const grown = std::realloc(list->names, new_capacity * sizeof(*list->names));
    if (!grown)
      return false;
    list->names = static_cast<char(*)[MaxFilenameLength + 1]>(grown);
    *capacity = new_capacity;
  }
  std::memcpy(list->names[list->count], name, length + 1);
  list->count++;
  return true;
}

ResultCode FileSystem::ReadDirectory(Uid uid, Gid gid, const char* path, DirectoryList* list)
{
  list->names = nullptr;
  list->count = 0;

  if (!IsValidPath(path))
    return ResultCode::Invalid;

  const FstEntry* entry = GetFstEntryForPath(path);
  if (!entry)
    return ResultCode::NotFound;

  if (!entry->CheckPermission(uid, gid, Mode::Read))
    return ResultCode::AccessDenied;

  if (entry->data.is_file)
    return ResultCode::Invalid;

  char host_path[HOST_PATH_SIZE];
  if (!BuildFilename(path, host_path, sizeof(host_path)))
    return ResultCode::Invalid;

  struct RDIR* const dir = retro_opendir_include_hidden(host_path, true);
  if (dir)
  {
    const bool is_root = !std::strcmp(path, "/");
    u32 capacity = 0;
    bool ok = true;
    while (ok && retro_readdir(dir))
    {
      const char* const name = retro_dirent_get_name(dir);
      if (!name || !std::strcmp(name, ".") || !std::strcmp(name, ".."))
        continue;
      // Drop files in the root of the Wii NAND folder, since we store extra files there that the
      // emulated system shouldn't know about.
      if (is_root && !retro_dirent_is_dir(dir, nullptr))
        continue;
      ok = AddDirectoryEntry(list, &capacity, name);
    }
    retro_closedir(dir);
    if (!ok)
    {
      FreeDirectoryList(list);
      return ResultCode::UnknownError;
    }
  }

  const u32 count = list->count;
  if (!count)
    return ResultCode::Success;

  // Sort files according to their order in the FST tree (issue 10234).
  // The result should look like this:
  //     [FilesNotInFst, ..., OldestFileInFst, ..., NewestFileInFst]
  // key: position in the FST; files that are not in the FST go first
  int* const keys = static_cast<int*>(
      std::malloc(count * (sizeof(int) + sizeof(u32)) + count * sizeof(*list->names)));
  if (!keys)
  {
    FreeDirectoryList(list);
    return ResultCode::UnknownError;
  }
  u32* const order = reinterpret_cast<u32*>(keys + count);
  char(*const names)[MaxFilenameLength + 1] =
      reinterpret_cast<char(*)[MaxFilenameLength + 1]>(order + count);
  for (u32 i = 0; i < count; i++)
  {
    keys[i] = -1;
    for (u32 j = 0; j < entry->child_count; j++)
    {
      if (!std::strcmp(entry->children[j].name, list->names[i]))
      {
        keys[i] = int(j);
        break;
      }
    }
  }

  // Insertion sort in reverse order because Nintendo traverses a linked list in
  // which new elements are inserted at the front. Files that are not in the FST
  // sort lexicographically so results do not depend on the host filesystem.
  for (u32 i = 0; i < count; i++)
  {
    u32 j = i;
    while (j > 0)
    {
      const u32 prev = order[j - 1];
      const int key1 = keys[i];
      const int key2 = keys[prev];
      const bool before =
          key1 != key2 ? key1 > key2 : std::strcmp(list->names[i], list->names[prev]) > 0;
      if (!before)
        break;
      order[j] = prev;
      j--;
    }
    order[j] = i;
  }

  std::memcpy(names, list->names, count * sizeof(*list->names));
  for (u32 i = 0; i < count; i++)
    std::memcpy(list->names[i], names[order[i]], sizeof(*list->names));
  std::free(keys);
  return ResultCode::Success;
}

Result<Metadata> FileSystem::GetMetadata(Uid uid, Gid gid, const char* path)
{
  const FstEntry* entry = nullptr;
  if (!std::strcmp(path, "/"))
  {
    entry = &m_root_entry;
  }
  else
  {
    if (!IsValidNonRootPath(path))
      return std::unexpected{ResultCode::Invalid};

    SplitPathResult split_path;
    SplitPathAndBasename(path, &split_path);
    const FstEntry* parent = GetFstEntryForPath(split_path.parent);
    if (!parent)
      return std::unexpected{ResultCode::NotFound};
    if (!parent->CheckPermission(uid, gid, Mode::Read))
      return std::unexpected{ResultCode::AccessDenied};
    entry = GetFstEntryForPath(path);
  }

  if (!entry)
    return std::unexpected{ResultCode::NotFound};

  Metadata metadata = entry->data;
  char host_path[HOST_PATH_SIZE];
  metadata.size = BuildFilename(path, host_path, sizeof(host_path)) ? File::GetSize(host_path) : 0;
  return metadata;
}

ResultCode FileSystem::SetMetadata(Uid caller_uid, const char* path, Uid uid, Gid gid,
                                   FileAttribute attr, Modes modes)
{
  if (!IsValidPath(path))
    return ResultCode::Invalid;

  FstEntry* entry = GetFstEntryForPath(path);
  if (!entry)
    return ResultCode::NotFound;

  if (caller_uid != 0 && caller_uid != entry->data.uid)
    return ResultCode::AccessDenied;
  if (caller_uid != 0 && uid != entry->data.uid)
    return ResultCode::AccessDenied;

  char host_path[HOST_PATH_SIZE];
  const bool is_empty =
      !BuildFilename(path, host_path, sizeof(host_path)) || File::GetSize(host_path) == 0;
  if (entry->data.uid != uid && entry->data.is_file && !is_empty)
    return ResultCode::FileNotEmpty;

  if (entry->data.gid != gid || entry->data.uid != uid || entry->data.attribute != attr ||
      entry->data.modes != modes)
  {
    entry->data.gid = gid;
    entry->data.uid = uid;
    entry->data.attribute = attr;
    entry->data.modes = modes;
    SaveFst();
  }

  return ResultCode::Success;
}

static u64 ComputeUsedClusters(const File::FSTEntry& parent_entry)
{
  u64 clusters = 0;
  for (const File::FSTEntry& entry : parent_entry.children)
  {
    if (entry.isDirectory)
      clusters += ComputeUsedClusters(entry);
    else
      clusters += Common::AlignUp(entry.size, CLUSTER_SIZE) / CLUSTER_SIZE;
  }
  return clusters;
}

Result<NandStats> FileSystem::GetNandStats()
{
  const auto root_stats = GetDirectoryStats("/");
  if (!root_stats)
    return std::unexpected{root_stats.error()};  // TODO: is this right? can this fail on hardware?

  NandStats stats{};
  stats.cluster_size = CLUSTER_SIZE;
  stats.free_clusters = USABLE_CLUSTERS - root_stats->used_clusters;
  stats.used_clusters = root_stats->used_clusters;
  stats.bad_clusters = 0;
  stats.reserved_clusters = RESERVED_CLUSTERS;
  stats.free_inodes = TOTAL_INODES - root_stats->used_inodes;
  stats.used_inodes = root_stats->used_inodes;

  return stats;
}

Result<DirectoryStats> FileSystem::GetDirectoryStats(const char* wii_path)
{
  const auto result = GetExtendedDirectoryStats(wii_path);
  if (!result)
    return std::unexpected{result.error()};

  DirectoryStats stats{};
  stats.used_inodes = static_cast<u32>(MIN((u64)result->used_inodes, (u64)TOTAL_INODES));
  stats.used_clusters = static_cast<u32>(MIN((u64)result->used_clusters, (u64)USABLE_CLUSTERS));
  return stats;
}

Result<ExtendedDirectoryStats> FileSystem::GetExtendedDirectoryStats(const char* wii_path)
{
  if (!IsValidPath(wii_path))
    return std::unexpected{ResultCode::Invalid};

  ExtendedDirectoryStats stats{};
  char path[HOST_PATH_SIZE];
  if (!BuildFilename(wii_path, path, sizeof(path)))
    return std::unexpected{ResultCode::Invalid};
  File::FileInfo info(path);
  if (!info.Exists())
  {
    return std::unexpected{ResultCode::NotFound};
  }
  if (info.IsDirectory())
  {
    File::FSTEntry parent_dir = File::ScanDirectoryTree(path, true);
    FixupDirectoryEntries(&parent_dir, !std::strcmp(wii_path, "/"));

    // add one for the folder itself
    stats.used_inodes = 1 + parent_dir.size;
    stats.used_clusters = ComputeUsedClusters(parent_dir);
  }
  else
  {
    return std::unexpected{ResultCode::Invalid};
  }
  return stats;
}
}  // namespace IOS::HLE::FS
