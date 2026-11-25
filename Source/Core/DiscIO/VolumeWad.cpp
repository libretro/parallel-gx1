// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeWad.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <retro_miscellaneous.h>
#include <string>
#include <utility>

#include <crypto/crypto.h>

#include <lrc_hash.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
VolumeWAD::~VolumeWAD()
{
  DestroyBlobReader(m_reader);
  std::free(m_cert_chain);
}

VolumeWAD::VolumeWAD(VolumeWAD&& other) noexcept
    : Volume(VolumeKind::WAD), m_reader(other.m_reader), m_ticket(std::move(other.m_ticket)),
      m_tmd(std::move(other.m_tmd)), m_cert_chain(std::exchange(other.m_cert_chain, nullptr)),
      m_cert_chain_offset(other.m_cert_chain_offset), m_ticket_offset(other.m_ticket_offset),
      m_tmd_offset(other.m_tmd_offset), m_data_offset(other.m_data_offset),
      m_opening_bnr_offset(other.m_opening_bnr_offset), m_hdr_size(other.m_hdr_size),
      m_cert_chain_size(other.m_cert_chain_size), m_ticket_size(other.m_ticket_size),
      m_tmd_size(other.m_tmd_size), m_data_size(other.m_data_size),
      m_opening_bnr_size(other.m_opening_bnr_size)
{
  other.m_reader = nullptr;
}

VolumeWAD::VolumeWAD(BlobReader* reader) : Volume(VolumeKind::WAD), m_reader(reader)
{
  ASSERT(m_reader);

  // Source: http://wiibrew.org/wiki/WAD_files
  if (!m_reader->ReadSwapped<u32>(0x00, &m_hdr_size))
    m_hdr_size = 0;
  if (!m_reader->ReadSwapped<u32>(0x08, &m_cert_chain_size))
    m_cert_chain_size = 0;
  if (!m_reader->ReadSwapped<u32>(0x10, &m_ticket_size))
    m_ticket_size = 0;
  if (!m_reader->ReadSwapped<u32>(0x14, &m_tmd_size))
    m_tmd_size = 0;
  if (!m_reader->ReadSwapped<u32>(0x18, &m_data_size))
    m_data_size = 0;
  if (!m_reader->ReadSwapped<u32>(0x1C, &m_opening_bnr_size))
    m_opening_bnr_size = 0;

  m_cert_chain_offset = Common::AlignUp(m_hdr_size, 0x40);
  m_ticket_offset = m_cert_chain_offset + Common::AlignUp(m_cert_chain_size, 0x40);
  m_tmd_offset = m_ticket_offset + Common::AlignUp(m_ticket_size, 0x40);
  m_data_offset = m_tmd_offset + Common::AlignUp(m_tmd_size, 0x40);
  m_opening_bnr_offset = m_data_offset + Common::AlignUp(m_data_size, 0x40);

  // read straight into the reader
  u8* const ticket_bytes = m_ticket.ResizeBytes(m_ticket_size);
  if (ticket_bytes)
    Read(m_ticket_offset, m_ticket_size, ticket_bytes);

  if (!IOS::ES::IsValidTMDSize(m_tmd_size))
  {
    ERROR_LOG(DISCIO, "TMD has an invalid size: %u bytes", m_tmd_size);
    return;
  }

  u8* const tmd_bytes = m_tmd.ResizeBytes(m_tmd_size);
  if (tmd_bytes)
    Read(m_tmd_offset, m_tmd_size, tmd_bytes);

  if (m_cert_chain_size)
  {
    m_cert_chain = static_cast<u8*>(std::calloc(m_cert_chain_size, 1));
    if (m_cert_chain)
      Read(m_cert_chain_offset, m_cert_chain_size, m_cert_chain);
  }
}

bool VolumeWAD::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  if (partition != PARTITION_NONE)
    return false;

  return m_reader->Read(offset, length, buffer);
}

const FileSystem* VolumeWAD::GetFileSystem(const Partition& partition) const
{
  // TODO: Implement this?
  return nullptr;
}

Region VolumeWAD::GetRegion() const
{
  if (!m_tmd.IsValid())
    return Region::Unknown;
  return m_tmd.GetRegion();
}

Country VolumeWAD::GetCountry(const Partition& partition) const
{
  if (!m_tmd.IsValid())
    return Country::Unknown;

  const u8 country_byte = static_cast<u8>(m_tmd.GetTitleId() & 0xff);
  if (country_byte == 2)  // SYSMENU
    return TypicalCountryForRegion(GetSysMenuRegion(m_tmd.GetTitleVersion()));

  const Region region = GetRegion();
  u16 revision_value;
  const u16* const revision =
      GetRevision(PARTITION_NONE, &revision_value) ? &revision_value : nullptr;
  if (CountryCodeToRegion(country_byte, Platform::WiiWAD, region, revision) != region)
    return TypicalCountryForRegion(region);

  return CountryCodeToCountry(country_byte, Platform::WiiWAD, region, revision);
}

const IOS::ES::TicketReader& VolumeWAD::GetTicket(const Partition& partition) const
{
  return m_ticket;
}

const IOS::ES::TMDReader& VolumeWAD::GetTMD(const Partition& partition) const
{
  return m_tmd;
}

const u8* VolumeWAD::GetCertificateChain(const Partition& partition, size_t* size) const
{
  *size = m_cert_chain ? m_cert_chain_size : 0;
  return m_cert_chain;
}

bool VolumeWAD::GetContentLocation(u16 index, u64* out_offset, u64* out_size) const
{
  u64 offset = m_data_offset;
  for (u16 i = 0; i < m_tmd.GetNumContents(); ++i)
  {
    IOS::ES::Content content;
    if (!m_tmd.GetContent(i, &content))
      break;
    const u64 aligned_size = Common::AlignUp(content.size, 0x40);
    if (content.index == index)
    {
      *out_offset = offset;
      *out_size = aligned_size;
      return true;
    }
    offset += aligned_size;
  }
  return false;
}

bool VolumeWAD::CheckContentIntegrity(const IOS::ES::Content& content, const u8* encrypted_data,
                                      size_t encrypted_size,
                                      const IOS::ES::TicketReader& ticket) const
{
  if (encrypted_size != Common::AlignUp(content.size, 0x40))
    return false;

  struct aes_ctx context;
  u8 title_key[16];
  ticket.GetTitleKey(title_key);
  if (aes_init(&context, title_key, 16))
    return false;

  // The chain carries on from chunk to chunk in iv
  u8 iv[AES_BLOCK_SIZE] = {};
  iv[0] = static_cast<u8>(content.index >> 8);
  iv[1] = static_cast<u8>(content.index & 0xFF);

  // Decrypt and hash in chunks, so the borrowed input is never copied whole
  struct sha1_state sha1;
  u8 digest[20];
  u8 chunk[2048];
  sha1_stream_init(&sha1);
  u64 left_to_hash = content.size;
  size_t offset = 0;
  while (offset < encrypted_size)
  {
    const size_t left = encrypted_size - offset;
    const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
    aes_cbc_decrypt(&context, iv, encrypted_data + offset, chunk, n);
    const size_t hashed = left_to_hash < n ? size_t(left_to_hash) : n;
    sha1_stream_update(&sha1, chunk, hashed);
    left_to_hash -= hashed;
    offset += n;
  }

  sha1_stream_final(&sha1, digest);
  return !std::memcmp(digest, content.sha1, sizeof(digest));
}

IOS::ES::TicketReader VolumeWAD::GetTicketWithFixedCommonKey() const
{
  if (!m_ticket.IsValid() || !m_tmd.IsValid())
    return m_ticket;

  size_t sig_size;
  const u8* const sig = m_ticket.GetSignatureData(&sig_size);
  size_t zeroes = 0;
  while (zeroes < sig_size && sig[zeroes] == 0)
    ++zeroes;
  if (zeroes != sig_size)
  {
    // This does not look like a typical "invalid common key index" ticket, so let's assume
    // the index is correct. This saves some time when reading properly signed titles.
    return m_ticket;
  }

  // Find the smallest content so that we spend as little time as possible in CheckContentIntegrity
  IOS::ES::Content smallest_content;
  if (!m_tmd.IsValid() || m_tmd.GetNumContents() == 0 || !m_tmd.GetContent(0, &smallest_content))
    return m_ticket;
  u64 offset_of_smallest_content = m_data_offset;

  u64 offset = m_data_offset;
  for (u16 i = 0; i < m_tmd.GetNumContents(); ++i)
  {
    IOS::ES::Content content;
    if (!m_tmd.GetContent(i, &content))
      break;
    if (content.size < smallest_content.size)
    {
      smallest_content = content;
      offset_of_smallest_content = offset;
    }
    offset += Common::AlignUp(content.size, 0x40);
  }

  const size_t content_size = size_t(Common::AlignUp(smallest_content.size, 0x40));
  u8* const content_data = static_cast<u8*>(std::malloc(content_size ? content_size : 1));
  if (!content_data)
    return m_ticket;
  if (!m_reader->Read(offset_of_smallest_content, content_size, content_data))
  {
    std::free(content_data);
    return m_ticket;
  }

  const u8 specified_index = m_ticket.GetCommonKeyIndex();
  if (specified_index < ARRAY_SIZE(IOS::HLE::IOSC::COMMON_KEY_HANDLES) &&
      CheckContentIntegrity(smallest_content, content_data, content_size, m_ticket))
  {
    std::free(content_data);
    return m_ticket;  // The common key index is already correct
  }

  // Try every common key index except the one we already tried
  IOS::ES::TicketReader new_ticket = m_ticket;
  for (u8 i = 0; i < ARRAY_SIZE(IOS::HLE::IOSC::COMMON_KEY_HANDLES); ++i)
  {
    if (i != specified_index)
    {
      new_ticket.OverwriteCommonKeyIndex(i);
      if (CheckContentIntegrity(smallest_content, content_data, content_size, new_ticket))
      {
        std::free(content_data);
        return new_ticket;  // We've found the common key index that should be used
      }
    }
  }
  std::free(content_data);

  ERROR_LOG(DISCIO, "Couldn't find valid common key for WAD file (%u specified)",
            (unsigned)specified_index);
  return m_ticket;
}

std::string VolumeWAD::GetGameID(const Partition& partition) const
{
  if (!m_tmd.IsValid())
    return {};

  return m_tmd.GetGameID();
}

std::string VolumeWAD::GetGameTDBID(const Partition& partition) const
{
  if (!m_tmd.IsValid())
    return {};

  return m_tmd.GetGameTDBID();
}

std::string VolumeWAD::GetMakerID(const Partition& partition) const
{
  char temp[2];
  if (!Read(0x198 + m_tmd_offset, 2, (u8*)temp, partition))
    return "00";

  // Some weird channels use 0x0000 in place of the MakerID, so we need a check here
  if (!Common::IsPrintableCharacter(temp[0]) || !Common::IsPrintableCharacter(temp[1]))
    return "00";

  return FilterGameID(temp, sizeof(temp));
}

bool VolumeWAD::GetTitleID(const Partition& partition, u64* title_id) const
{
  return ReadSwapped<u64>(m_ticket_offset + 0x01DC, partition, title_id);
}

bool VolumeWAD::GetRevision(const Partition& partition, u16* revision) const
{
  if (!m_tmd.IsValid())
    return false;

  *revision = m_tmd.GetTitleVersion();
  return true;
}

Platform VolumeWAD::GetVolumeType() const
{
  return Platform::WiiWAD;
}

bool VolumeWAD::IsDatelDisc() const
{
  return false;
}

bool VolumeWAD::IsNKit() const
{
  return false;
}

BlobType VolumeWAD::GetBlobType() const
{
  return m_reader->GetBlobType();
}

u64 VolumeWAD::GetDataSize() const
{
  return m_reader->GetDataSize();
}

DataSizeType VolumeWAD::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeWAD::GetRawSize() const
{
  return m_reader->GetRawSize();
}

const BlobReader& VolumeWAD::GetBlobReader() const
{
  return *m_reader;
}

}  // namespace DiscIO
