// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/Volume.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/IOS/ES/Formats.h"

#include "DiscIO/Blob.h"
#include "DiscIO/CachedBlob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeGC.h"
#include "DiscIO/VolumeWad.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
const IOS::ES::TicketReader Volume::INVALID_TICKET{};
const IOS::ES::TMDReader Volume::INVALID_TMD{};

// f called with the volume as the class it is
template <typename V, typename F>
static decltype(auto) AsOwnClass(V* volume, F&& f)
{
  constexpr bool is_const = std::is_const_v<V>;
  switch (volume->GetKind())
  {
  case VolumeKind::GC:
    return f(static_cast<std::conditional_t<is_const, const VolumeGC*, VolumeGC*>>(volume));
  case VolumeKind::Wii:
    return f(static_cast<std::conditional_t<is_const, const VolumeWii*, VolumeWii*>>(volume));
  case VolumeKind::WAD:
  default:
    return f(static_cast<std::conditional_t<is_const, const VolumeWAD*, VolumeWAD*>>(volume));
  }
}

// Whether a class has its own Name rather than Volume's default
#define HAS_OWN(cls, Name)                                                                         \
  (!std::is_same_v<decltype(&std::remove_cv_t<cls>::Name), decltype(&Volume::Name)>)

void DestroyVolume(Volume* volume)
{
  if (!volume)
    return;
  AsOwnClass(volume, [](auto* v) { delete v; });
}

bool Volume::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  return AsOwnClass(this, [&](auto* v) { return v->Read(offset, length, buffer, partition); });
}

const u8* Volume::View(u64 offset, u64 length, const Partition& partition) const
{
  return AsOwnClass(this, [&](auto* v) -> const u8* {
    using Cls = std::remove_cv_t<std::remove_pointer_t<decltype(v)>>;
    if constexpr (HAS_OWN(Cls, View))
      return v->View(offset, length, partition);
    else
      return nullptr;
  });
}

void Volume::Prefetch(u64 offset, u64 length, const Partition& partition) const
{
  AsOwnClass(this, [&](auto* v) {
    using Cls = std::remove_cv_t<std::remove_pointer_t<decltype(v)>>;
    if constexpr (HAS_OWN(Cls, Prefetch))
      v->Prefetch(offset, length, partition);
  });
}

// these have a default here, which a class without its own has
#define DEFAULTED(type, Name, params, args, default_value)                                         \
  type Volume::Name params const                                                                   \
  {                                                                                                \
    return AsOwnClass(this, [&](auto* v) -> type {                                                 \
      using Cls = std::remove_cv_t<std::remove_pointer_t<decltype(v)>>;                            \
      if constexpr (HAS_OWN(Cls, Name))                                                            \
        return v->Name args;                                                                       \
      else                                                                                         \
        return default_value;                                                                      \
    });                                                                                            \
  }

DEFAULTED(bool, HasWiiHashes, (), (), false)
DEFAULTED(size_t, GetPartitionCount, (), (), 0)
DEFAULTED(Partition, GetPartition, (size_t index), (index), PARTITION_NONE)
DEFAULTED(Partition, GetGamePartition, (), (), PARTITION_NONE)
DEFAULTED(const IOS::ES::TicketReader&, GetTicket, (const Partition& partition), (partition),
          INVALID_TICKET)
DEFAULTED(const IOS::ES::TMDReader&, GetTMD, (const Partition& partition), (partition), INVALID_TMD)
const u8* Volume::GetCertificateChain(const Partition& partition, size_t* size) const
{
  *size = 0;
  return AsOwnClass(this, [&](auto* v) -> const u8* {
    using Cls = std::remove_cv_t<std::remove_pointer_t<decltype(v)>>;
    if constexpr (HAS_OWN(Cls, GetCertificateChain))
      return v->GetCertificateChain(partition, size);
    else
      return nullptr;
  });
}
DEFAULTED(bool, GetContentLocation, (u16 index, u64* offset, u64* size), (index, offset, size),
          false)
DEFAULTED(bool, CheckContentIntegrity,
          (const IOS::ES::Content& content, const u8* encrypted_data, size_t encrypted_size,
           const IOS::ES::TicketReader& ticket),
          (content, encrypted_data, encrypted_size, ticket), false)
DEFAULTED(IOS::ES::TicketReader, GetTicketWithFixedCommonKey, (), (), {})
DEFAULTED(u64, PartitionOffsetToRawOffset, (u64 offset, const Partition& partition),
          (offset, partition), offset)

#undef DEFAULTED

// these every class has
#define OWN(type, Name, params, args)                                                              \
  type Volume::Name params const                                                                   \
  {                                                                                                \
    return AsOwnClass(this, [&](auto* v) -> type { return v->Name args; });                        \
  }

OWN(const FileSystem*, GetFileSystem, (const Partition& partition), (partition))
OWN(std::string, GetGameID, (const Partition& partition), (partition))
OWN(std::string, GetGameTDBID, (const Partition& partition), (partition))
OWN(std::string, GetMakerID, (const Partition& partition), (partition))
OWN(bool, GetRevision, (const Partition& partition, u16* revision), (partition, revision))
OWN(Platform, GetVolumeType, (), ())
OWN(bool, IsDatelDisc, (), ())
OWN(bool, IsNKit, (), ())
OWN(Region, GetRegion, (), ())
OWN(Country, GetCountry, (const Partition& partition), (partition))
OWN(u32, GetSimulatedMemorySize, (), ())
OWN(BlobType, GetBlobType, (), ())
OWN(u64, GetDataSize, (), ())
OWN(DataSizeType, GetDataSizeType, (), ())
OWN(u64, GetRawSize, (), ())
OWN(const BlobReader&, GetBlobReader, (), ())

#undef OWN

// overloaded, so named here by class: the Wii disc and the WAD have their own
bool Volume::GetTitleID(const Partition& partition, u64* title_id) const
{
  switch (m_kind)
  {
  case VolumeKind::Wii:
    return static_cast<const VolumeWii*>(this)->GetTitleID(partition, title_id);
  case VolumeKind::WAD:
    return static_cast<const VolumeWAD*>(this)->GetTitleID(partition, title_id);
  default:
    return false;
  }
}

u32 Volume::GetOffsetShift() const
{
  if (m_kind != VolumeKind::Wii)
    return 0;
  return static_cast<const VolumeWii*>(this)->GetOffsetShift();
}

std::string Volume::FilterGameID(const char* data, size_t size)
{
  std::string string(data, size);

  // We don't want game IDs to contain characters that are unprintable or might cause path
  // traversal. Game IDs normally only contain ASCII uppercase letters and numbers,
  // but GNHE5d contains a lowercase letter, so let's allow all ASCII letters and numbers.
  for (char& c : string)
  {
    if (!Common::IsAlnum(c))
      c = '-';
  }

  return string;
}

// A volume over reader, which it takes (reader is then nullptr), adapted by reader_adapter_factory;
// nullptr with reader left as it is when it is not a disc.
template <typename T = std::identity>
static VolumeDisc* TryCreateDisc(BlobReader*& reader, const T& reader_adapter_factory = {})
{
  if (!reader)
    return nullptr;

  // `reader_adapter_factory` is used *after* successful magic word read.
  // This prevents `CachedBlobReader` from showing warnings when failing to scrub a .dol file.

  u32 magic;
  if (reader->ReadSwapped<u32>(0x18, &magic) && magic == WII_DISC_MAGIC)
  {
    BlobReader* const taken = reader;
    reader = nullptr;
    return new VolumeWii(reader_adapter_factory(taken));
  }

  if (reader->ReadSwapped<u32>(0x1C, &magic) && magic == GAMECUBE_DISC_MAGIC)
  {
    BlobReader* const taken = reader;
    reader = nullptr;
    return new VolumeGC(reader_adapter_factory(taken));
  }

  // No known magic words found
  return nullptr;
}

VolumeDisc* CreateDisc(BlobReader* reader)
{
  VolumeDisc* disc = TryCreateDisc(reader);
  DestroyBlobReader(reader);
  return disc;
}

VolumeDisc* CreateDisc(const std::string& path)
{
  return CreateDisc(CreateBlobReader(path));
}

VolumeDisc* CreateDiscForCore(const std::string& path)
{
  BlobReader* reader = CreateBlobReader(path);

  VolumeDisc* disc = Config::Get(Config::MAIN_LOAD_GAME_INTO_MEMORY) ?
                         TryCreateDisc(reader, CreateScrubbingCachedBlobReader) :
                         TryCreateDisc(reader);
  DestroyBlobReader(reader);
  return disc;
}

// As TryCreateDisc
static VolumeWAD* TryCreateWAD(BlobReader*& reader)
{
  if (!reader)
    return nullptr;

  // Check for WAD
  // 0x206962 for boot2 wads
  u32 wad_magic;
  if (reader->ReadSwapped<u32>(0x02, &wad_magic) &&
      (wad_magic == u32(0x00204973) || wad_magic == u32(0x00206962)))
  {
    BlobReader* const taken = reader;
    reader = nullptr;
    return new VolumeWAD(taken);
  }

  // No known magic words found
  return nullptr;
}

VolumeWAD* CreateWAD(BlobReader* reader)
{
  VolumeWAD* wad = TryCreateWAD(reader);
  DestroyBlobReader(reader);
  return wad;
}

VolumeWAD* CreateWAD(const std::string& path)
{
  return CreateWAD(CreateBlobReader(path));
}

Volume* CreateVolume(BlobReader* reader)
{
  VolumeDisc* disc = TryCreateDisc(reader);
  if (disc)
    return disc;

  VolumeWAD* wad = TryCreateWAD(reader);
  DestroyBlobReader(reader);
  if (wad)
    return wad;

  return nullptr;
}
}  // namespace DiscIO
