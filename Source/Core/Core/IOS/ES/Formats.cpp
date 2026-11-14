// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/ES/Formats.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdio>

#include <lrc_hash.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Projection.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"

namespace IOS::ES
{
constexpr size_t CONTENT_VIEW_SIZE = 0x10;

bool IsTitleType(u64 title_id, TitleType title_type)
{
  return static_cast<u32>(title_id >> 32) == static_cast<u32>(title_type);
}

bool IsDiscTitle(u64 title_id)
{
  return IsTitleType(title_id, TitleType::Game) ||
         IsTitleType(title_id, TitleType::GameWithChannel);
}

bool IsChannel(u64 title_id)
{
  if (title_id == Titles::SYSTEM_MENU)
    return true;

  return IsTitleType(title_id, TitleType::Channel) ||
         IsTitleType(title_id, TitleType::SystemChannel) ||
         IsTitleType(title_id, TitleType::GameWithChannel) ||
         IsTitleType(title_id, TitleType::HiddenChannel);
}

bool Content::IsShared() const
{
  return (type & 0x8000) != 0;
}

bool Content::IsOptional() const
{
  return (type & 0x4000) != 0;
}

bool operator==(const Content& lhs, const Content& rhs)
{
  auto fields = [](const Content& c) { return std::tie(c.id, c.index, c.type, c.size, c.sha1); };
  return fields(lhs) == fields(rhs);
}

SignedBlobReader::SignedBlobReader(const u8* bytes, size_t size)
{
  SetBytes(bytes, size);
}

SignedBlobReader::SignedBlobReader(const SignedBlobReader& other)
{
  SetBytes(other.m_bytes, other.m_size);
}

SignedBlobReader::SignedBlobReader(SignedBlobReader&& other) noexcept
    : m_bytes(other.m_bytes), m_size(other.m_size)
{
  other.m_bytes = nullptr;
  other.m_size = 0;
}

SignedBlobReader& SignedBlobReader::operator=(const SignedBlobReader& other)
{
  if (this != &other)
    SetBytes(other.m_bytes, other.m_size);
  return *this;
}

SignedBlobReader& SignedBlobReader::operator=(SignedBlobReader&& other) noexcept
{
  if (this != &other)
  {
    std::free(m_bytes);
    m_bytes = other.m_bytes;
    m_size = other.m_size;
    other.m_bytes = nullptr;
    other.m_size = 0;
  }
  return *this;
}

SignedBlobReader::~SignedBlobReader()
{
  std::free(m_bytes);
}

void SignedBlobReader::SetBytes(const u8* bytes, size_t size)
{
  u8* const dst = ResizeBytes(size);
  if (dst)
    std::memmove(dst, bytes, size);
}

u8* SignedBlobReader::ResizeBytes(size_t size)
{
  if (!size)
  {
    std::free(m_bytes);
    m_bytes = nullptr;
    m_size = 0;
    return nullptr;
  }
  if (size != m_size)
  {
    u8* const bytes = static_cast<u8*>(std::realloc(m_bytes, size));
    if (!bytes)
    {
      std::free(m_bytes);
      m_bytes = nullptr;
      m_size = 0;
      return nullptr;
    }
    m_bytes = bytes;
    m_size = size;
  }
  return m_bytes;
}

static size_t GetIssuerOffset(SignatureType signature_type)
{
  switch (signature_type)
  {
  case SignatureType::RSA2048:
    return offsetof(SignatureRSA2048, issuer);
  case SignatureType::RSA4096:
    return offsetof(SignatureRSA4096, issuer);
  case SignatureType::ECC:
    return offsetof(SignatureECC, issuer);
  default:
    return 0;
  }
}

std::array<u8, 20> SignedBlobReader::GetSha1() const
{
  const size_t skip = GetIssuerOffset(GetSignatureType());
  std::array<u8, 20> digest;
  SHA1Digest(m_bytes + skip, m_size - skip, digest.data());
  return digest;
}

bool SignedBlobReader::IsSignatureValid() const
{
  // Too small for the certificate type.
  if (m_size < sizeof(SignatureType))
    return false;

  // Too small to contain the whole signature data.
  const size_t signature_size = GetSignatureSize();
  if (signature_size == 0 || m_size < signature_size)
    return false;

  return true;
}

SignatureType SignedBlobReader::GetSignatureType() const
{
  return static_cast<SignatureType>(Common::swap32(m_bytes));
}

const u8* SignedBlobReader::GetSignatureData(size_t* size) const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    *size = sizeof(SignatureRSA4096::sig);
    return m_bytes + offsetof(SignatureRSA4096, sig);
  case SignatureType::RSA2048:
    *size = sizeof(SignatureRSA2048::sig);
    return m_bytes + offsetof(SignatureRSA2048, sig);
  case SignatureType::ECC:
    *size = sizeof(SignatureECC::sig);
    return m_bytes + offsetof(SignatureECC, sig);
  default:
    *size = 0;
    return nullptr;
  }
}

size_t SignedBlobReader::GetSignatureSize() const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    return sizeof(SignatureRSA4096);
  case SignatureType::RSA2048:
    return sizeof(SignatureRSA2048);
  case SignatureType::ECC:
    return sizeof(SignatureECC);
  default:
    return 0;
  }
}

template <typename T>
static std::string DetailGetIssuer(const u8* bytes)
{
  const char* issuer = reinterpret_cast<const char*>(bytes + offsetof(T, issuer));
  return {issuer, strnlen(issuer, sizeof(T::issuer))};
}

std::string SignedBlobReader::GetIssuer() const
{
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    return DetailGetIssuer<SignatureRSA4096>(m_bytes);
  case SignatureType::RSA2048:
    return DetailGetIssuer<SignatureRSA2048>(m_bytes);
  case SignatureType::ECC:
    return DetailGetIssuer<SignatureECC>(m_bytes);
  default:
    return "";
  }
}

void SignedBlobReader::DoState(PointerWrap& p)
{
  // laid out as a std::vector<u8> was: the size, then the bytes
  u32 size = static_cast<u32>(m_size);
  p.Do(size);
  if (p.IsReadMode())
    ResizeBytes(size);
  if (m_size && m_size == size)
    p.DoArray(m_bytes, size);
}

bool IsValidTMDSize(size_t size)
{
  return size >= sizeof(TMDHeader) && size <= 0x49e4;
}

TMDReader::TMDReader(const u8* bytes, size_t size) : SignedBlobReader(bytes, size)
{
}

bool TMDReader::IsValid() const
{
  if (!IsSignatureValid())
    return false;

  if (m_size < sizeof(TMDHeader))
  {
    // TMD is too small to contain its base fields.
    return false;
  }

  if (m_size < sizeof(TMDHeader) + GetNumContents() * sizeof(Content))
  {
    // TMD is too small to contain all its expected content entries.
    return false;
  }

  return true;
}

size_t TMDReader::GetRawViewSize() const
{
  return (offsetof(TMDHeader, access_rights) - offsetof(TMDHeader, tmd_version)) +
         sizeof(TMDHeader::title_version) + sizeof(TMDHeader::num_contents) +
         size_t(GetNumContents()) * CONTENT_VIEW_SIZE;
}

void TMDReader::GetRawView(u8* out) const
{
  // Base fields
  const size_t base = offsetof(TMDHeader, access_rights) - offsetof(TMDHeader, tmd_version);
  std::memcpy(out, m_bytes + offsetof(TMDHeader, tmd_version), base);
  out += base;
  std::memcpy(out, m_bytes + offsetof(TMDHeader, title_version), sizeof(TMDHeader::title_version));
  out += sizeof(TMDHeader::title_version);
  std::memcpy(out, m_bytes + offsetof(TMDHeader, num_contents), sizeof(TMDHeader::num_contents));
  out += sizeof(TMDHeader::num_contents);

  // Content views (same as Content, but without the hash)
  const size_t num_contents = GetNumContents();
  for (size_t i = 0; i < num_contents; ++i)
  {
    std::memcpy(out, m_bytes + sizeof(TMDHeader) + i * sizeof(Content), CONTENT_VIEW_SIZE);
    out += CONTENT_VIEW_SIZE;
  }
}

u16 TMDReader::GetBootIndex() const
{
  return Common::swap16(m_bytes + offsetof(TMDHeader, boot_index));
}

u64 TMDReader::GetIOSId() const
{
  return Common::swap64(m_bytes + offsetof(TMDHeader, ios_id));
}

u64 TMDReader::GetTitleId() const
{
  return Common::swap64(m_bytes + offsetof(TMDHeader, title_id));
}

u32 TMDReader::GetTitleFlags() const
{
  return Common::swap32(m_bytes + offsetof(TMDHeader, title_flags));
}

u16 TMDReader::GetTitleVersion() const
{
  return Common::swap16(m_bytes + offsetof(TMDHeader, title_version));
}

u16 TMDReader::GetGroupId() const
{
  return Common::swap16(m_bytes + offsetof(TMDHeader, group_id));
}

DiscIO::Region TMDReader::GetRegion() const
{
  if (!IsChannel(GetTitleId()))
    return DiscIO::Region::Unknown;

  if (GetTitleId() == Titles::SYSTEM_MENU)
    return DiscIO::GetSysMenuRegion(GetTitleVersion());

  const DiscIO::Region region =
      static_cast<DiscIO::Region>(Common::swap16(m_bytes + offsetof(TMDHeader, region)));

  return region <= DiscIO::Region::NTSC_K ? region : DiscIO::Region::Unknown;
}

std::string TMDReader::GetGameID() const
{
  char game_id[6];
  std::memcpy(game_id, m_bytes + offsetof(TMDHeader, title_id) + 4, 4);
  std::memcpy(game_id + 4, m_bytes + offsetof(TMDHeader, group_id), 2);

  size_t i;
  for (i = 0; i < sizeof(game_id); i++)
  {
    if (!Common::IsAlnum(game_id[i]))
      break;
  }
  if (i == sizeof(game_id))
    return std::string(game_id, sizeof(game_id));

  return StringFromFormat("%016" PRIx64, (u64)GetTitleId());
}

std::string TMDReader::GetGameTDBID() const
{
  const u8* begin = m_bytes + offsetof(TMDHeader, title_id) + 4;
  size_t i;
  for (i = 0; i < 4; i++)
  {
    if (!Common::IsAlnum(static_cast<char>(begin[i])))
      break;
  }
  if (i == 4)
    return std::string(reinterpret_cast<const char*>(begin), 4);

  return StringFromFormat("%016" PRIx64, (u64)GetTitleId());
}

u16 TMDReader::GetNumContents() const
{
  return Common::swap16(m_bytes + offsetof(TMDHeader, num_contents));
}

bool TMDReader::GetContent(u16 index, Content* content) const
{
  if (!IsValid() || index >= GetNumContents())
  {
    return false;
  }

  const u8* content_base = m_bytes + sizeof(TMDHeader) + index * sizeof(Content);
  content->id = Common::swap32(content_base + offsetof(Content, id));
  content->index = Common::swap16(content_base + offsetof(Content, index));
  content->type = Common::swap16(content_base + offsetof(Content, type));
  content->size = Common::swap64(content_base + offsetof(Content, size));
  std::memcpy(content->sha1.data(), content_base + offsetof(Content, sha1), content->sha1.size());

  return true;
}

std::vector<Content> TMDReader::GetContents() const
{
  std::vector<Content> contents(IsValid() ? GetNumContents() : 0);
  for (size_t i = 0; i < contents.size(); ++i)
    GetContent(static_cast<u16>(i), &contents[i]);
  return contents;
}

bool TMDReader::FindContentById(u32 id, Content* content) const
{
  for (u16 index = 0; index < GetNumContents(); ++index)
  {
    if (!GetContent(index, content))
    {
      return false;
    }
    if (content->id == id)
    {
      return true;
    }
  }
  return false;
}

TicketReader::TicketReader(const u8* bytes, size_t size) : SignedBlobReader(bytes, size)
{
}

bool TicketReader::IsValid() const
{
  if (!IsSignatureValid() || !m_size)
    return false;

  if (IsV1Ticket())
    return m_size == GetTicketSize();

  return m_size % sizeof(Ticket) == 0;
}

bool TicketReader::IsV1Ticket() const
{
  // Version can only be 0 or 1.
  return GetVersion() == 1;
}

size_t TicketReader::GetNumberOfTickets() const
{
  if (IsV1Ticket())
    return 1;

  return m_size / sizeof(Ticket);
}

u32 TicketReader::GetTicketSize() const
{
  if (IsV1Ticket())
  {
    return Common::swap32(m_bytes + sizeof(Ticket) +
                          offsetof(V1TicketHeader, v1_ticket_size)) +
           sizeof(Ticket);
  }

  return sizeof(Ticket);
}

const u8* TicketReader::GetRawTicket(u64 ticket_id_to_find, size_t* size) const
{
  const size_t ticket_size = GetTicketSize();
  const size_t num_tickets = GetNumberOfTickets();
  for (size_t i = 0; i < num_tickets; ++i)
  {
    const u8* const ticket_begin = m_bytes + ticket_size * i;
    const u64 ticket_id = Common::swap64(ticket_begin + offsetof(ES::Ticket, ticket_id));
    if (ticket_id == ticket_id_to_find)
    {
      *size = ticket_size;
      return ticket_begin;
    }
  }
  *size = 0;
  return nullptr;
}

void TicketReader::GetRawTicketView(u32 ticket_num, u8* out) const
{
  GetRawTicketView(m_bytes, ticket_num, out);
}

void TicketReader::GetRawTicketView(const u8* tickets, u32 ticket_num, u8* out)
{
  // A ticket view is composed of a version + part of a ticket starting from the ticket_id field.
  const u8* const ticket_start = tickets + sizeof(Ticket) * ticket_num;
  out[0] = tickets[offsetof(Ticket, version)];
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;

  // Copy the rest of the ticket view structure from the ticket.
  std::memcpy(out + sizeof(u32), ticket_start + offsetof(Ticket, ticket_id),
              sizeof(TicketView) - sizeof(u32));
}

u8 TicketReader::GetVersion() const
{
  return m_bytes[offsetof(Ticket, version)];
}

u32 TicketReader::GetDeviceId() const
{
  return Common::swap32(m_bytes + offsetof(Ticket, device_id));
}

u64 TicketReader::GetTitleId() const
{
  return Common::swap64(m_bytes + offsetof(Ticket, title_id));
}

u8 TicketReader::GetCommonKeyIndex() const
{
  return m_bytes[offsetof(Ticket, common_key_index)];
}

std::array<u8, 16> TicketReader::GetTitleKey(const HLE::IOSC& iosc) const
{
  u8 iv[16] = {};
  std::memcpy(iv, m_bytes + offsetof(Ticket, title_id), sizeof(Ticket::title_id));

  u8 index = m_bytes[offsetof(Ticket, common_key_index)];
  if (index >= HLE::IOSC::COMMON_KEY_HANDLES.size())
  {
    PanicAlert("Bad common key index for title %016" PRIx64 ": %u -- using common key 0",
               (u64)GetTitleId(), (unsigned)index);
    index = 0;
  }
  auto common_key_handle = HLE::IOSC::COMMON_KEY_HANDLES[index];

  std::array<u8, 16> key;
  iosc.Decrypt(common_key_handle, iv, &m_bytes[offsetof(Ticket, title_key)], 16, key.data(),
               PID_ES);
  return key;
}

std::array<u8, 16> TicketReader::GetTitleKey() const
{
  return GetTitleKey(HLE::IOSC{GetConsoleType()});
}

HLE::IOSC::ConsoleType TicketReader::GetConsoleType() const
{
  const bool is_rvt = GetIssuer() == "Root-CA00000002-XS00000006";
  return is_rvt ? HLE::IOSC::ConsoleType::RVT : HLE::IOSC::ConsoleType::Retail;
}

void TicketReader::DeleteTicket(u64 ticket_id_to_delete)
{
  // the tickets kept close up in place
  const size_t num_tickets = GetNumberOfTickets();
  const size_t ticket_size = GetTicketSize();
  size_t kept = 0;
  for (size_t i = 0; i < num_tickets; ++i)
  {
    const u8* const ticket_start = m_bytes + ticket_size * i;
    const u64 ticket_id = Common::swap64(ticket_start + offsetof(Ticket, ticket_id));
    if (ticket_id == ticket_id_to_delete)
      continue;
    if (kept != i)
      std::memmove(m_bytes + ticket_size * kept, ticket_start, ticket_size);
    ++kept;
  }
  ResizeBytes(ticket_size * kept);
}

HLE::ReturnCode TicketReader::Unpersonalise(HLE::IOSC& iosc)
{
  const auto ticket_begin = m_bytes;

  // IOS uses IOSC to compute an AES key from the peer public key and the device's private ECC key,
  // which is used the decrypt the title key. The IV is the ticket ID (8 bytes), zero extended.
  using namespace HLE;
  IOSC::Handle public_handle;
  ReturnCode ret =
      iosc.CreateObject(&public_handle, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  const auto public_key_iter = ticket_begin + offsetof(Ticket, server_public_key);
  ret = iosc.ImportPublicKey(public_handle, public_key_iter, nullptr, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  IOSC::Handle key_handle;
  ret = iosc.CreateObject(&key_handle, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  ret = iosc.ComputeSharedKey(key_handle, IOSC::HANDLE_CONSOLE_KEY, public_handle, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  std::array<u8, 16> iv{};
  std::memcpy(iv.data(), ticket_begin + offsetof(Ticket, ticket_id), sizeof(Ticket::ticket_id));

  std::array<u8, 16> key{};
  ret = iosc.Decrypt(key_handle, iv.data(), ticket_begin + offsetof(Ticket, title_key),
                     sizeof(Ticket::title_key), key.data(), PID_ES);
  // Finally, IOS copies the decrypted title key back to the ticket buffer.
  if (ret == IPC_SUCCESS)
    std::memcpy(ticket_begin + offsetof(Ticket, title_key), key.data(), key.size());

  return ret;
}

void TicketReader::OverwriteCommonKeyIndex(u8 index)
{
  m_bytes[offsetof(Ticket, common_key_index)] = index;
}

struct SharedContentMap::Entry
{
  // ID string
  std::array<u8, 8> id;
  // Binary SHA1 hash
  std::array<u8, 20> sha1;
};

constexpr char CONTENT_MAP_PATH[] = "/shared1/content.map";
SharedContentMap::SharedContentMap(HLE::FSCore& fs_core) : m_fs{fs_core.GetFS()}
{
  static_assert(sizeof(Entry) == 28, "SharedContentMap::Entry has the wrong size");

  Entry entry;
  const auto fd =
      fs_core.Open(PID_KERNEL, PID_KERNEL, CONTENT_MAP_PATH, HLE::FS::Mode::Read, {}, &m_ticks);
  if (fd.Get() < 0)
    return;
  while (fs_core.Read(fd.Get(), &entry, 1, &m_ticks) == sizeof(entry))
  {
    m_entries.push_back(entry);
    m_last_id++;
  }
}

SharedContentMap::~SharedContentMap() = default;

std::optional<std::string>
SharedContentMap::GetFilenameFromSHA1(const std::array<u8, 20>& sha1) const
{
  const Entry* it = nullptr;
  for (const Entry& entry : m_entries)
  {
    if (std::memcmp(entry.sha1.data(), sha1.data(), sha1.size()) == 0)
    {
      it = &entry;
      break;
    }
  }
  if (!it)
    return {};

  const std::string_view id_string(reinterpret_cast<const char*>(it->id.data()), it->id.size());
  return StringFromFormat("/shared1/%.*s.app", static_cast<int>(id_string.size()),
                          id_string.data());
}

std::vector<std::array<u8, 20>> SharedContentMap::GetHashes() const
{
  std::vector<std::array<u8, 20>> hashes;
  hashes.reserve(m_entries.size());
  for (const auto& content_entry : m_entries)
    hashes.emplace_back(content_entry.sha1);

  return hashes;
}

std::string SharedContentMap::AddSharedContent(const std::array<u8, 20>& sha1)
{
  if (auto filename = GetFilenameFromSHA1(sha1))
    return *std::move(filename);

  Entry& entry = m_entries.emplace_back();
  static_assert(sizeof(m_last_id) == 4,
                "'m_last_id' must be represented by 8 characters when formatted in hexadecimal.");
  static_assert(std::tuple_size_v<decltype(entry.id)> == sizeof(m_last_id) * 2,
                "'entry.id' must be a std::array capable of storing every nibble of 'm_last_id'.");
  char id[9];
  snprintf(id, sizeof(id), "%08x", m_last_id);
  std::memcpy(entry.id.data(), id, entry.id.size());
  entry.sha1 = sha1;

  WriteEntries();
  m_last_id++;

  const std::string_view id_string(reinterpret_cast<const char*>(entry.id.data()), entry.id.size());
  return StringFromFormat("/shared1/%.*s.app", static_cast<int>(id_string.size()),
                          id_string.data());
}

bool SharedContentMap::DeleteSharedContent(const std::array<u8, 20>& sha1)
{
  std::erase_if(m_entries, [&sha1](const auto& entry) { return entry.sha1 == sha1; });
  return WriteEntries();
}

bool SharedContentMap::WriteEntries() const
{
  // Temporary files are only 12 characters long and must match the final file name
  const std::string temp_path = "/tmp/content.map";
  // Atomically write the new content map.
  {
    constexpr HLE::FS::Modes modes{HLE::FS::Mode::ReadWrite, HLE::FS::Mode::ReadWrite,
                                   HLE::FS::Mode::None};
    const auto file = m_fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, temp_path, modes);
    if (!file || !file->Write(m_entries.data(), m_entries.size()))
      return false;
  }
  return m_fs->Rename(PID_KERNEL, PID_KERNEL, temp_path, CONTENT_MAP_PATH) ==
         HLE::FS::ResultCode::Success;
}

static std::pair<u32, u64> ReadUidSysEntry(HLE::FSCore& fs, u64 fd, u64* ticks)
{
  u64 title_id = 0;
  if (fs.Read(fd, &title_id, 1, ticks) != sizeof(title_id))
    return {};

  u32 uid = 0;
  if (fs.Read(fd, &uid, 1, ticks) != sizeof(uid))
    return {};

  return {Common::swap32(uid), Common::swap64(title_id)};
}

constexpr char UID_MAP_PATH[] = "/sys/uid.sys";
UIDSys::UIDSys(HLE::FSCore& fs_core) : m_fs{fs_core.GetFS()}
{
  if (const auto fd =
          fs_core.Open(PID_KERNEL, PID_KERNEL, UID_MAP_PATH, HLE::FS::Mode::Read, {}, &m_ticks);
      fd.Get() >= 0)
  {
    while (true)
    {
      std::pair<u32, u64> entry = ReadUidSysEntry(fs_core, fd.Get(), &m_ticks);
      if (!entry.first && !entry.second)
        break;

      m_entries.insert(std::move(entry));
    }
  }

  if (m_entries.empty())
  {
    GetOrInsertUIDForTitle(Titles::SYSTEM_MENU);
  }
}

u32 UIDSys::GetUIDFromTitle(u64 title_id) const
{
  for (const auto& entry : m_entries)
  {
    if (entry.second == title_id)
      return entry.first;
  }
  return 0;
}

u32 UIDSys::GetNextUID() const
{
  if (m_entries.empty())
    return FIRST_PPC_UID;
  return m_entries.rbegin()->first + 1;
}

u32 UIDSys::GetOrInsertUIDForTitle(const u64 title_id)
{
  const u32 current_uid = GetUIDFromTitle(title_id);
  if (current_uid)
  {
    INFO_LOG(IOS_ES, "Title %016" PRIx64 " already exists in uid.sys", (u64)title_id);
    return current_uid;
  }

  const u32 uid = GetNextUID();
  m_entries.insert({uid, title_id});

  // Byte swap before writing.
  const u64 swapped_title_id = Common::swap64(title_id);
  const u32 swapped_uid = Common::swap32(uid);

  constexpr HLE::FS::Modes modes{HLE::FS::Mode::ReadWrite, HLE::FS::Mode::ReadWrite,
                                 HLE::FS::Mode::None};
  const auto file = m_fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, UID_MAP_PATH, modes);
  if (!file || !file->Seek(0, HLE::FS::SeekMode::End) || !file->Write(&swapped_title_id, 1) ||
      !file->Write(&swapped_uid, 1))
  {
    ERROR_LOG(IOS_ES, "Failed to write to /sys/uid.sys");
    return 0;
  }

  return uid;
}

CertReader::CertReader(const u8* data, size_t size)
{
  // the signature type and public key type are read where the bytes lie,
  // and only the certificate they make is copied
  if (size < sizeof(u32))
    return;
  const SignatureType signature_type = static_cast<SignatureType>(Common::swap32(data));
  size_t signature_size;
  switch (signature_type)
  {
  case SignatureType::RSA4096:
    signature_size = sizeof(SignatureRSA4096);
    break;
  case SignatureType::RSA2048:
    signature_size = sizeof(SignatureRSA2048);
    break;
  case SignatureType::ECC:
    signature_size = sizeof(SignatureECC);
    break;
  default:
    return;
  }
  if (size < signature_size + offsetof(CertHeader, public_key_type) + sizeof(u32))
    return;
  const PublicKeyType public_key_type = static_cast<PublicKeyType>(
      Common::swap32(data + signature_size + offsetof(CertHeader, public_key_type)));

  size_t cert_size;
  if (signature_type == SignatureType::RSA4096 && public_key_type == PublicKeyType::RSA2048)
    cert_size = sizeof(CertRSA4096RSA2048);
  else if (signature_type == SignatureType::RSA2048 && public_key_type == PublicKeyType::RSA2048)
    cert_size = sizeof(CertRSA2048RSA2048);
  else if (signature_type == SignatureType::RSA2048 && public_key_type == PublicKeyType::ECC)
    cert_size = sizeof(CertRSA2048ECC);
  else if (signature_type == SignatureType::ECC && public_key_type == PublicKeyType::ECC)
    cert_size = sizeof(CertECC);
  else
    return;
  if (size < cert_size)
    return;

  SetBytes(data, cert_size);
  m_is_valid = m_size == cert_size;
}

bool CertReader::IsValid() const
{
  return m_is_valid;
}

u32 CertReader::GetId() const
{
  const size_t offset = GetSignatureSize() + offsetof(CertHeader, id);
  return Common::swap32(m_bytes + offset);
}

std::string CertReader::GetName() const
{
  const char* name = reinterpret_cast<const char*>(m_bytes + GetSignatureSize() +
                                                   offsetof(CertHeader, name));
  return std::string(name, strnlen(name, sizeof(CertHeader::name)));
}

PublicKeyType CertReader::GetPublicKeyType() const
{
  const size_t offset = GetSignatureSize() + offsetof(CertHeader, public_key_type);
  return static_cast<PublicKeyType>(Common::swap32(m_bytes + offset));
}

const u8* CertReader::GetPublicKey(size_t* size) const
{
  // RSA keys carry their 4-byte exponent after them
  switch (GetSignatureType())
  {
  case SignatureType::RSA4096:
    *size = sizeof(CertRSA4096RSA2048::public_key) + 4;
    return m_bytes + offsetof(CertRSA4096RSA2048, public_key);
  case SignatureType::RSA2048:
    if (GetPublicKeyType() == PublicKeyType::RSA2048)
    {
      *size = sizeof(CertRSA2048RSA2048::public_key) + 4;
      return m_bytes + offsetof(CertRSA2048RSA2048, public_key);
    }
    *size = sizeof(CertRSA2048ECC::public_key);
    return m_bytes + offsetof(CertRSA2048ECC, public_key);
  case SignatureType::ECC:
    *size = sizeof(CertECC::public_key);
    return m_bytes + offsetof(CertECC, public_key);
  default:
    *size = 0;
    return nullptr;
  }
}

std::map<std::string, CertReader> ParseCertChain(const u8* chain, size_t size)
{
  std::map<std::string, CertReader> certs;

  size_t processed = 0;
  while (processed != size)
  {
    CertReader cert_reader{chain + processed, size - processed};
    if (!cert_reader.IsValid())
      return certs;

    size_t cert_size;
    cert_reader.GetBytes(&cert_size);
    processed += cert_size;
    std::string name = cert_reader.GetName();
    certs.emplace(std::move(name), std::move(cert_reader));
  }
  return certs;
}
}  // namespace IOS::ES
