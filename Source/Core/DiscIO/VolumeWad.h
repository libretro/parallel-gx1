// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class Country;
class FileSystem;
enum class Language;
enum class Region;
enum class Platform;

class VolumeWAD final : public Volume
{
public:
  // takes reader
  VolumeWAD(BlobReader* reader);
  ~VolumeWAD();
  VolumeWAD(const VolumeWAD&) = delete;
  VolumeWAD& operator=(const VolumeWAD&) = delete;
  // the reader goes with it (boot parameters hold a WAD by value)
  VolumeWAD(VolumeWAD&& other) noexcept;
  VolumeWAD& operator=(VolumeWAD&&) = delete;
  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition = PARTITION_NONE) const;
  const FileSystem* GetFileSystem(const Partition& partition = PARTITION_NONE) const;
  std::optional<u64> GetTitleID(const Partition& partition = PARTITION_NONE) const;
  const IOS::ES::TicketReader& GetTicket(const Partition& partition = PARTITION_NONE) const;
  const IOS::ES::TMDReader& GetTMD(const Partition& partition = PARTITION_NONE) const;
  const std::vector<u8>& GetCertificateChain(const Partition& partition = PARTITION_NONE) const;
  std::vector<u8> GetContent(u16 index) const;
  bool CheckContentIntegrity(const IOS::ES::Content& content, const u8* encrypted_data,
                             size_t encrypted_size, const IOS::ES::TicketReader& ticket) const;
  IOS::ES::TicketReader GetTicketWithFixedCommonKey() const;
  std::string GetGameID(const Partition& partition = PARTITION_NONE) const;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const;
  std::string GetMakerID(const Partition& partition = PARTITION_NONE) const;
  std::optional<u16> GetRevision(const Partition& partition = PARTITION_NONE) const;
  Platform GetVolumeType() const;
  bool IsDatelDisc() const;
  bool IsNKit() const;
  Region GetRegion() const;
  Country GetCountry(const Partition& partition = PARTITION_NONE) const;
  u32 GetSimulatedMemorySize() const { return 0; }

  BlobType GetBlobType() const;
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;
  u64 GetRawSize() const;
  const BlobReader& GetBlobReader() const;

private:
  // Volume reaches the functions it has of its own here
  friend class Volume;

  BlobReader* m_reader;  // owned
  IOS::ES::TicketReader m_ticket;
  IOS::ES::TMDReader m_tmd;
  std::vector<u8> m_cert_chain;
  u32 m_cert_chain_offset = 0;
  u32 m_ticket_offset = 0;
  u32 m_tmd_offset = 0;
  u32 m_data_offset = 0;
  u32 m_opening_bnr_offset = 0;
  u32 m_hdr_size = 0;
  u32 m_cert_chain_size = 0;
  u32 m_ticket_size = 0;
  u32 m_tmd_size = 0;
  u32 m_data_size = 0;
  u32 m_opening_bnr_size = 0;
};

}  // namespace DiscIO
