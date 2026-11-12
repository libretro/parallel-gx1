// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/Volume.h"

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
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
const std::vector<u8> Volume::INVALID_CERT_CHAIN{};

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

template <typename T = std::identity>
static std::unique_ptr<VolumeDisc> TryCreateDisc(std::unique_ptr<BlobReader>& reader,
                                                 const T& reader_adapter_factory = {})
{
  if (!reader)
    return nullptr;

  // `reader_adapter_factory` is used *after* successful magic word read.
  // This prevents `CachedBlobReader` from showing warnings when failing to scrub a .dol file.

  if (reader->ReadSwapped<u32>(0x18) == WII_DISC_MAGIC)
    return std::make_unique<VolumeWii>(reader_adapter_factory(std::move(reader)));

  if (reader->ReadSwapped<u32>(0x1C) == GAMECUBE_DISC_MAGIC)
    return std::make_unique<VolumeGC>(reader_adapter_factory(std::move(reader)));

  // No known magic words found
  return nullptr;
}

std::unique_ptr<VolumeDisc> CreateDisc(std::unique_ptr<BlobReader> reader)
{
  return TryCreateDisc(reader);
}

std::unique_ptr<VolumeDisc> CreateDisc(const std::string& path)
{
  return CreateDisc(CreateBlobReader(path));
}

std::unique_ptr<VolumeDisc> CreateDiscForCore(const std::string& path)
{
  auto reader = CreateBlobReader(path);

  if (Config::Get(Config::MAIN_LOAD_GAME_INTO_MEMORY))
    return TryCreateDisc(reader, CreateScrubbingCachedBlobReader);

  return TryCreateDisc(reader);
}

static std::unique_ptr<VolumeWAD> TryCreateWAD(std::unique_ptr<BlobReader>& reader)
{
  if (!reader)
    return nullptr;

  // Check for WAD
  // 0x206962 for boot2 wads
  const std::optional<u32> wad_magic = reader->ReadSwapped<u32>(0x02);
  if (wad_magic == u32(0x00204973) || wad_magic == u32(0x00206962))
    return std::make_unique<VolumeWAD>(std::move(reader));

  // No known magic words found
  return nullptr;
}

std::unique_ptr<VolumeWAD> CreateWAD(std::unique_ptr<BlobReader> reader)
{
  return TryCreateWAD(reader);
}

std::unique_ptr<VolumeWAD> CreateWAD(const std::string& path)
{
  return CreateWAD(CreateBlobReader(path));
}

std::unique_ptr<Volume> CreateVolume(std::unique_ptr<BlobReader> reader)
{
  std::unique_ptr<VolumeDisc> disc = TryCreateDisc(reader);
  if (disc)
    return disc;

  std::unique_ptr<VolumeWAD> wad = TryCreateWAD(reader);
  if (wad)
    return wad;

  return nullptr;
}

std::unique_ptr<Volume> CreateVolume(const std::string& path)
{
  return CreateVolume(CreateBlobReader(path));
}
}  // namespace DiscIO
