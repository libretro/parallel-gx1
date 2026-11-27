// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/SysConf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <retro_miscellaneous.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

constexpr size_t SYSCONF_SIZE = 0x4000;

static size_t GetNonArrayEntrySize(SysConf::Entry::Type type)
{
  switch (type)
  {
  case SysConf::Entry::Type::Byte:
  case SysConf::Entry::Type::ByteBool:
    return 1;
  case SysConf::Entry::Type::Short:
    return 2;
  case SysConf::Entry::Type::Long:
    return 4;
  case SysConf::Entry::Type::LongLong:
    return 8;
  default:
    ASSERT(false);
    return 0;
  }
}
SysConf::SysConf(IOS::HLE::FS::FileSystem* fs) : m_fs{fs}
{
  Load();
}

SysConf::~SysConf()
{
  Save();
  Clear();
  std::free(m_entries);
}

void SysConf::Clear()
{
  for (u32 i = 0; i < m_entry_count; i++)
    std::free(m_entries[i].bytes);
  m_entry_count = 0;
}

void SysConf::Load()
{
  Clear();

  const auto file = m_fs->OpenFile(IOS::HLE::FS::Uid{0}, IOS::HLE::FS::Gid{0},
                                   "/shared2/sys/SYSCONF", IOS::HLE::FS::Mode::Read);
  if (!file || file->GetSize() != SYSCONF_SIZE || !LoadFromFile(*file))
  {
    WARN_LOG(CORE, "No valid SYSCONF detected. Creating a new one.");
    InsertDefaultEntries();
  }
}

bool SysConf::LoadFromFile(const IOS::HLE::FS::FileHandle& file)
{
  (void)file.Seek(4, IOS::HLE::FS::SeekMode::Set);
  u16 number_of_entries;
  (void)file.Read(&number_of_entries, 1);
  number_of_entries = Common::swap16(number_of_entries);

  u16* const offsets = static_cast<u16*>(std::malloc((number_of_entries + 1) * sizeof(u16)));
  if (!offsets)
    return false;
  for (u32 i = 0; i < number_of_entries; i++)
  {
    (void)file.Read(&offsets[i], 1);
    offsets[i] = Common::swap16(offsets[i]);
  }

  bool ok = true;
  for (u32 i = 0; ok && i < number_of_entries; i++)
  {
    const u16 offset = offsets[i];
    (void)file.Seek(offset, IOS::HLE::FS::SeekMode::Set);

    // Metadata
    u8 description = 0;
    (void)file.Read(&description, 1);
    const Entry::Type type = static_cast<Entry::Type>((description & 0xe0) >> 5);
    const u8 name_length = (description & 0x1f) + 1;
    char name[MAX_NAME_LENGTH + 1] = {};
    (void)file.Read(name, name_length);

    // Data
    u32 data_size;
    switch (type)
    {
    case Entry::Type::BigArray:
    {
      u16 data_length = 0;
      (void)file.Read(&data_length, 1);
      // The stored u16 is length - 1, not length.
      data_size = u32(Common::swap16(data_length)) + 1;
      break;
    }
    case Entry::Type::SmallArray:
    {
      u8 data_length = 0;
      (void)file.Read(&data_length, 1);
      data_size = u32(data_length) + 1;
      break;
    }
    case Entry::Type::Byte:
    case Entry::Type::ByteBool:
    case Entry::Type::Short:
    case Entry::Type::Long:
    case Entry::Type::LongLong:
      data_size = static_cast<u32>(GetNonArrayEntrySize(type));
      break;
    default:
      ERROR_LOG(CORE, "Unknown entry type %u in SYSCONF for %s (offset %u)",
                (unsigned)(static_cast<u8>(type)), name, (unsigned)offset);
      ok = false;
      continue;
    }

    Entry* const entry = AddEntry(type, name, nullptr, data_size);
    if (!entry)
    {
      ok = false;
      continue;
    }
    (void)file.Read(entry->bytes, data_size);
  }
  std::free(offsets);
  return ok;
}

// Writes size bytes at offset, keeping only what fits in the SYSCONF; returns the next offset.
static size_t PutBytes(u8* buffer, size_t offset, const void* data, size_t size)
{
  if (offset < SYSCONF_SIZE)
    std::memcpy(buffer + offset, data, size < SYSCONF_SIZE - offset ? size : SYSCONF_SIZE - offset);
  return offset + size;
}

static size_t PutU16(u8* buffer, size_t offset, u16 value)
{
  const u16 swapped_value = Common::swap16(value);
  return PutBytes(buffer, offset, &swapped_value, sizeof(swapped_value));
}

bool SysConf::Save() const
{
  // Laid out as: header, entry offsets and a past-the-end one, entries, then the footer in the
  // last bytes of the file. Anything that does not fit is cut off.
  u8* const buffer = static_cast<u8*>(std::calloc(1, SYSCONF_SIZE));
  if (!buffer)
    return false;

  // Header
  static constexpr u8 version[4]{'S', 'C', 'v', '0'};
  size_t offset_position = PutBytes(buffer, 0, version, sizeof(version));
  offset_position = PutU16(buffer, offset_position, static_cast<u16>(m_entry_count));

  const size_t entries_begin_offset = offset_position + sizeof(u16) * (m_entry_count + 1);
  size_t entry_position = entries_begin_offset;
  for (u32 i = 0; i < m_entry_count; i++)
  {
    const Entry& item = m_entries[i];
    const size_t name_size = std::strlen(item.name);

    // Offset
    offset_position = PutU16(buffer, offset_position, static_cast<u16>(entry_position));

    // Entry metadata (type and name)
    const u8 description =
        static_cast<u8>((static_cast<u8>(item.type) << 5) | (static_cast<u8>(name_size) - 1));
    entry_position = PutBytes(buffer, entry_position, &description, 1);
    entry_position = PutBytes(buffer, entry_position, item.name, name_size);

    // Entry data
    switch (item.type)
    {
    case Entry::Type::BigArray:
    {
      const u16 data_size = static_cast<u16>(item.size);
      // length - 1 is stored, not length.
      entry_position = PutU16(buffer, entry_position, static_cast<u16>(data_size - 1));
      entry_position = PutBytes(buffer, entry_position, item.bytes, data_size);
      break;
    }

    case Entry::Type::SmallArray:
    {
      const u8 data_size = static_cast<u8>(item.size);
      const u8 stored_size = static_cast<u8>(data_size - 1);
      entry_position = PutBytes(buffer, entry_position, &stored_size, 1);
      entry_position = PutBytes(buffer, entry_position, item.bytes, data_size);
      break;
    }

    default:
      entry_position = PutBytes(buffer, entry_position, item.bytes, item.size);
      break;
    }
  }
  // Offset for the dummy past-the-end entry.
  PutU16(buffer, offset_position, static_cast<u16>(entry_position));

  static constexpr u8 footer[4] = {'S', 'C', 'e', 'd'};
  std::memcpy(buffer + SYSCONF_SIZE - sizeof(footer), footer, sizeof(footer));

  // Write the new data.
  static constexpr char temp_file[] = "/tmp/SYSCONF";
  constexpr auto rw_mode = IOS::HLE::FS::Mode::ReadWrite;
  bool written;
  {
    auto file = m_fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, temp_file,
                                        {rw_mode, rw_mode, rw_mode});
    written = file && file->Write(buffer, SYSCONF_SIZE);
  }
  std::free(buffer);
  if (!written)
    return false;
  m_fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, "/shared2/sys", 0,
                        {rw_mode, rw_mode, rw_mode});
  const auto result =
      m_fs->Rename(IOS::SYSMENU_UID, IOS::SYSMENU_GID, temp_file, "/shared2/sys/SYSCONF");
  return result == IOS::HLE::FS::ResultCode::Success;
}

SysConf::Entry* SysConf::AddEntry(Entry::Type type, const char* name, const u8* data, u32 size)
{
  if (!data && !size && type != Entry::Type::SmallArray && type != Entry::Type::BigArray)
    size = static_cast<u32>(GetNonArrayEntrySize(type));

  if (m_entry_count == m_entry_capacity)
  {
    const u32 capacity = m_entry_capacity ? m_entry_capacity * 2 : 32;
    Entry* const grown = static_cast<Entry*>(std::realloc(m_entries, capacity * sizeof(Entry)));
    if (!grown)
      return nullptr;
    m_entries = grown;
    m_entry_capacity = capacity;
  }

  u8* bytes = nullptr;
  if (size)
  {
    bytes = static_cast<u8*>(data ? std::malloc(size) : std::calloc(1, size));
    if (!bytes)
      return nullptr;
    if (data)
      std::memcpy(bytes, data, size);
  }

  Entry& entry = m_entries[m_entry_count++];
  entry.type = type;
  std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  entry.bytes = bytes;
  entry.size = size;
  return &entry;
}

bool SysConf::ResizeEntry(Entry* entry, u32 size)
{
  if (size == entry->size)
    return true;
  u8* const bytes = static_cast<u8*>(std::realloc(entry->bytes, size ? size : 1));
  if (!bytes)
    return false;
  if (size > entry->size)
    std::memset(bytes + entry->size, 0, size - entry->size);
  entry->bytes = bytes;
  entry->size = size;
  return true;
}

SysConf::Entry* SysConf::GetEntry(const char* key)
{
  for (u32 i = 0; i < m_entry_count; i++)
  {
    if (!std::strcmp(m_entries[i].name, key))
      return &m_entries[i];
  }
  return nullptr;
}

const SysConf::Entry* SysConf::GetEntry(const char* key) const
{
  for (u32 i = 0; i < m_entry_count; i++)
  {
    if (!std::strcmp(m_entries[i].name, key))
      return &m_entries[i];
  }
  return nullptr;
}

SysConf::Entry* SysConf::GetOrAddEntry(const char* key, Entry::Type type)
{
  if (Entry* entry = GetEntry(key))
    return entry;

  return AddEntry(type, key, nullptr, 0);
}

void SysConf::InsertDefaultEntries()
{
  static const u8 one[] = {1};
  static const u8 zero[] = {0};
  AddEntry(Entry::Type::BigArray, "BT.DINF", nullptr, 0x460 + 1);
  AddEntry(Entry::Type::BigArray, "BT.CDIF", nullptr, 0x204 + 1);
  static const u8 bt_sens[] = {0, 0, 0, 3};
  AddEntry(Entry::Type::Long, "BT.SENS", bt_sens, sizeof(bt_sens));
  AddEntry(Entry::Type::Byte, "BT.BAR", one, 1);
  static const u8 bt_spkv[] = {0x58};
  AddEntry(Entry::Type::Byte, "BT.SPKV", bt_spkv, 1);
  AddEntry(Entry::Type::Byte, "BT.MOT", one, 1);

  // 22 bytes: 2 bytes per character (10 characters maximum),
  // 1 for a null terminating character, 1 for the string length
  u8 console_nick[22] = {0, 'd', 0, 'o', 0, 'l', 0, 'p', 0, 'h', 0, 'i', 0, 'n'};
  console_nick[21] = static_cast<u8>(strlen("dolphin"));
  AddEntry(Entry::Type::SmallArray, "IPL.NIK", console_nick, sizeof(console_nick));

  AddEntry(Entry::Type::Byte, "IPL.LNG", one, 1);
  if (Entry* const ipl_sadr = AddEntry(Entry::Type::BigArray, "IPL.SADR", nullptr, 0x1007 + 1))
    ipl_sadr->bytes[0] = 0x6c;

  if (Entry* const ipl_pc = AddEntry(Entry::Type::SmallArray, "IPL.PC", nullptr, 0x49 + 1))
  {
    ipl_pc->bytes[1] = 0x04;
    ipl_pc->bytes[2] = 0x14;
  }

  static const u8 long_zero[] = {0, 0, 0, 0};
  AddEntry(Entry::Type::Long, "IPL.CB", long_zero, sizeof(long_zero));
  AddEntry(Entry::Type::Byte, "IPL.AR", one, 1);
  AddEntry(Entry::Type::Byte, "IPL.SSV", one, 1);

  AddEntry(Entry::Type::ByteBool, "IPL.CD", zero, 1);
  AddEntry(Entry::Type::ByteBool, "IPL.CD2", zero, 1);
  AddEntry(Entry::Type::ByteBool, "IPL.EULA", one, 1);
  static const u8 ipl_upt[] = {2};
  AddEntry(Entry::Type::Byte, "IPL.UPT", ipl_upt, 1);
  AddEntry(Entry::Type::Byte, "IPL.PGS", zero, 1);
  AddEntry(Entry::Type::Byte, "IPL.E60", one, 1);
  AddEntry(Entry::Type::Byte, "IPL.DH", zero, 1);
  static const u8 ipl_inc[] = {0, 0, 0, 8};
  AddEntry(Entry::Type::Long, "IPL.INC", ipl_inc, sizeof(ipl_inc));
  static const u8 ipl_frc[] = {0, 0, 0, 0x28};
  AddEntry(Entry::Type::Long, "IPL.FRC", ipl_frc, sizeof(ipl_frc));
  static const u8 ipl_idl[] = {0, 1};
  AddEntry(Entry::Type::SmallArray, "IPL.IDL", ipl_idl, sizeof(ipl_idl));

  static const u8 net_wcfg[] = {0, 0, 0, 1};
  AddEntry(Entry::Type::Long, "NET.WCFG", net_wcfg, sizeof(net_wcfg));
  AddEntry(Entry::Type::Long, "NET.CTPC", long_zero, sizeof(long_zero));
  AddEntry(Entry::Type::Byte, "WWW.RST", zero, 1);

  AddEntry(Entry::Type::ByteBool, "MPLS.MOVIE", one, 1);
}
