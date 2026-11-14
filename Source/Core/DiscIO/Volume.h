// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Enums.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class DataSizeType;
class FileSystem;
class VolumeDisc;
class VolumeWAD;

struct Partition final
{
  constexpr Partition() = default;
  constexpr explicit Partition(u64 offset_) : offset(offset_) {}
  constexpr bool operator==(const Partition& other) const { return offset == other.offset; }
  constexpr auto operator<=>(const Partition other) const { return offset <=> other.offset; }
  u64 offset{std::numeric_limits<u64>::max()};
};

constexpr Partition PARTITION_NONE(std::numeric_limits<u64>::max() - 1);

// The class a Volume is
enum class VolumeKind : u8
{
  GC,
  Wii,
  WAD,
};

// A GameCube or Wii disc (VolumeGC, VolumeWii, both a VolumeDisc) or a WAD (VolumeWAD). Which one
// is noted when it is made, and its functions go to that class through a branch on that, not a
// table of virtual functions; the ones with a default here go to it only where it has its own.
// One is owned through a plain pointer and freed with DestroyVolume, as the class it is.
class Volume
{
public:
  Volume(const Volume&) = delete;
  Volume& operator=(const Volume&) = delete;

  VolumeKind GetKind() const { return m_kind; }

  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const;
  // What Read() would put in buffer, lent from the image instead, valid while this volume
  // lives; nullptr when it cannot be lent (compressed, encrypted or assembled images).
  // Thread-safe.
  const u8* View(u64 offset, u64 length, const Partition& partition) const;
  // Asks the OS to start reading what View() lends; returns at once.
  void Prefetch(u64 offset, u64 length, const Partition& partition) const;
  template <typename T>
  std::optional<T> ReadSwapped(u64 offset, const Partition& partition) const
  {
    T temp;
    if (!Read(offset, sizeof(T), reinterpret_cast<u8*>(&temp), partition))
      return std::nullopt;
    return Common::FromBigEndian(temp);
  }
  std::optional<u64> ReadSwappedAndShifted(u64 offset, const Partition& partition) const
  {
    const std::optional<u32> temp = ReadSwapped<u32>(offset, partition);
    if (!temp)
      return std::nullopt;
    return static_cast<u64>(*temp) << GetOffsetShift();
  }

  bool HasWiiHashes() const;
  std::vector<Partition> GetPartitions() const;
  Partition GetGamePartition() const;
  std::optional<u64> GetTitleID() const { return GetTitleID(GetGamePartition()); }
  std::optional<u64> GetTitleID(const Partition& partition) const;
  const IOS::ES::TicketReader& GetTicket(const Partition& partition) const;
  const IOS::ES::TMDReader& GetTMD(const Partition& partition) const;
  const std::vector<u8>& GetCertificateChain(const Partition& partition) const;
  std::vector<u8> GetContent(u16 index) const;
  bool CheckContentIntegrity(const IOS::ES::Content& content, const u8* encrypted_data,
                             size_t encrypted_size, const IOS::ES::TicketReader& ticket) const;
  IOS::ES::TicketReader GetTicketWithFixedCommonKey() const;
  // Returns a non-owning pointer. Returns nullptr if the file system couldn't be read.
  const FileSystem* GetFileSystem(const Partition& partition) const;
  u64 PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const;
  std::string GetGameID(const Partition& partition = PARTITION_NONE) const;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const;
  std::string GetMakerID(const Partition& partition = PARTITION_NONE) const;
  std::optional<u16> GetRevision(const Partition& partition = PARTITION_NONE) const;
  Platform GetVolumeType() const;
  bool IsDatelDisc() const;
  bool IsNKit() const;
  bool CheckBlockIntegrity(u64 block_index, const u8* encrypted_data,
                           const Partition& partition) const;
  bool CheckBlockIntegrity(u64 block_index, const Partition& partition) const;
  Region GetRegion() const;
  Country GetCountry(const Partition& partition = PARTITION_NONE) const;
  u32 GetSimulatedMemorySize() const;
  BlobType GetBlobType() const;
  // Size of virtual disc (may be inaccurate depending on the blob type)
  u64 GetDataSize() const;
  DataSizeType GetDataSizeType() const;
  // Size on disc (compressed size)
  u64 GetRawSize() const;
  const BlobReader& GetBlobReader() const;

protected:
  explicit Volume(VolumeKind kind) : m_kind(kind) {}
  // not virtual: DestroyVolume destroys a volume as the class it is
  ~Volume() = default;

  static std::string FilterGameID(const char* data, size_t size);

  // the shift of offsets in the disc's headers: the Wii's own, 0 elsewhere
  u32 GetOffsetShift() const;

  static constexpr size_t NUMBER_OF_LANGUAGES = 10;
  static constexpr size_t NAME_CHARS_LENGTH = 42;
  static constexpr size_t NAME_BYTES_LENGTH = NAME_CHARS_LENGTH * sizeof(char16_t);
  static constexpr size_t NAMES_TOTAL_CHARS = NAME_CHARS_LENGTH * NUMBER_OF_LANGUAGES;
  static constexpr size_t NAMES_TOTAL_BYTES = NAME_BYTES_LENGTH * NUMBER_OF_LANGUAGES;

  static const IOS::ES::TicketReader INVALID_TICKET;
  static const IOS::ES::TMDReader INVALID_TMD;
  static const std::vector<u8> INVALID_CERT_CHAIN;

private:
  const VolumeKind m_kind;
};

// Destroys a volume as the class it is and frees it; nullptr does nothing.
void DestroyVolume(Volume* volume);

// A volume made is the caller's, freed with DestroyVolume; nullptr if none could be. These take
// the reader they are given, a volume made or not.
VolumeDisc* CreateDisc(BlobReader* reader);
VolumeDisc* CreateDisc(const std::string& path);
// This version enables caching when the "Load Games into Memory" setting is enabled.
VolumeDisc* CreateDiscForCore(const std::string& path);

VolumeWAD* CreateWAD(BlobReader* reader);
VolumeWAD* CreateWAD(const std::string& path);
Volume* CreateVolume(BlobReader* reader);
Volume* CreateVolume(const std::string& path);

}  // namespace DiscIO
