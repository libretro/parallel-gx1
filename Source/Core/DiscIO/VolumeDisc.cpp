// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/VolumeDisc.h"

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeGC.h"

namespace DiscIO
{
std::string VolumeDisc::GetGameID(const Partition& partition) const
{
  char id[6]{};

  if (GetVolumeType() == Platform::Triforce)
  {
    // Triforce games have their Game ID stored in the boot.id file
    const BootID* boot_id = static_cast<const VolumeGC*>(this)->GetTriforceBootID();

    // Construct game ID from the BTID
    id[0] = 'G';

    memcpy(id + 1, boot_id->game_id.data() + 2, 2);

    switch (GetCountry())
    {
    default:
    case Country::Japan:
      id[3] = 'J';
      break;
    case Country::Taiwan:
      id[3] = 'W';
      break;
    case Country::USA:
      id[3] = 'E';
      break;
    case Country::Europe:
      id[3] = 'P';
      break;
    }

    const std::string maker_id{GetMakerID()};
    memcpy(id + 4, maker_id.c_str(), maker_id.size() < 2 ? maker_id.size() : 2);

    return FilterGameID(id, sizeof(id));
  }

  if (!Read(0, sizeof(id), reinterpret_cast<u8*>(id), partition))
    return std::string();

  return FilterGameID(id, sizeof(id));
}

Country VolumeDisc::GetCountry(const Partition& partition) const
{
  u8 country_byte = 0;

  if (GetVolumeType() == Platform::Triforce)
  {
    const BootID* boot_id = static_cast<const VolumeGC*>(this)->GetTriforceBootID();

    switch (boot_id->region_flags & 0x000000FF)
    {
    default:
    case 0x02:  // JAPAN
      country_byte = 'J';
      break;
    case 0x08:  // ASIA
      country_byte = 'W';
      break;
    case 0x0E:  // USA
      country_byte = 'E';
      break;
    case 0x0C:  // EXPORT
      country_byte = 'P';
      break;
    }
  }
  else
  {
    // The 0 that we use as a default value is mapped to Country::Unknown and Region::Unknown
    country_byte = ReadSwapped<u8>(3, partition).value_or(0);
  }

  const Region region = GetRegion();
  const std::optional<u16> revision = GetRevision();

  if (CountryCodeToRegion(country_byte, GetVolumeType(), region, revision) != region)
    return TypicalCountryForRegion(region);

  return CountryCodeToCountry(country_byte, GetVolumeType(), region, revision);
}

Region VolumeDisc::RegionCodeToRegion(std::optional<u32> region_code) const
{
  if (!region_code)
    return Region::Unknown;

  const Region region = static_cast<Region>(*region_code);
  return region <= Region::NTSC_K ? region : Region::Unknown;
}

std::string VolumeDisc::GetMakerID(const Partition& partition) const
{
  char maker_id[2];

  // Triforce games have their maker stored in the boot.id file as an actual string
  if (GetVolumeType() == Platform::Triforce)
  {
    const BootID* boot_id = static_cast<const VolumeGC*>(this)->GetTriforceBootID();

    // There only seem to be three different makers here,
    // so we can just check the first char to difference between them.
    //
    // NAMCO CORPORATION, SEGA CORPORATION and Hitmaker co,ltd.

    switch (boot_id->manufacturer[0])
    {
    case 'S':  // SEGA CORPORATION
    case 'H':  // Hitmaker co,ltd
      return "6E";
    case 'N':  // NAMCO CORPORATION
      return "82";
    default:
      break;
    }
    // Fall back to normal maker from header
  }

  if (!Read(0x4, sizeof(maker_id), reinterpret_cast<u8*>(&maker_id), partition))
    return std::string();

  return FilterGameID(maker_id, sizeof(maker_id));
}

std::optional<u16> VolumeDisc::GetRevision(const Partition& partition) const
{
  std::optional<u8> revision = ReadSwapped<u8>(7, partition);
  return revision ? *revision : std::optional<u16>();
}

u32 VolumeDisc::GetSimulatedMemorySize() const
{
  // Simulated memory size is a 32-bit unsigned integer in the `bi2.bin` file at offset `0x04`.
  // Apploader reads this and "simulates" a smaller memory size. Originally useful for simulating
  // 24 MiB of memory on devkits with 48 MiB.
  return ReadSwapped<u32>(BI2_ADDRESS + 0x04, GetGamePartition()).value_or(0);
}

bool VolumeDisc::IsNKit() const
{
  constexpr u32 NKIT_MAGIC = 0x4E4B4954;  // "NKIT"
  return ReadSwapped<u32>(0x200, PARTITION_NONE) == NKIT_MAGIC;
}

}  // namespace DiscIO
