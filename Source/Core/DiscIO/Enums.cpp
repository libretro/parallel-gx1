// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/Enums.h"

#include <map>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace DiscIO
{
std::string GetName(Country country, bool translate)
{
  std::string name;

  switch (country)
  {
  case Country::Europe:
    name = _trans("Europe");
    break;
  case Country::Japan:
    name = _trans("Japan");
    break;
  case Country::USA:
    name = _trans("USA");
    break;
  case Country::Australia:
    name = _trans("Australia");
    break;
  case Country::France:
    name = _trans("France");
    break;
  case Country::Germany:
    name = _trans("Germany");
    break;
  case Country::Italy:
    name = _trans("Italy");
    break;
  case Country::Korea:
    name = _trans("Korea");
    break;
  case Country::Netherlands:
    name = _trans("Netherlands");
    break;
  case Country::Russia:
    name = _trans("Russia");
    break;
  case Country::Spain:
    name = _trans("Spain");
    break;
  case Country::Taiwan:
    name = _trans("Taiwan");
    break;
  case Country::World:
    name = _trans("World");
    break;
  default:
    name = _trans("Unknown");
    break;
  }

  return translate ? Common::GetStringT(name.c_str()) : name;
}

std::string GetName(Language language, bool translate)
{
  std::string name;

  switch (language)
  {
  case Language::Japanese:
    name = _trans("Japanese");
    break;
  case Language::English:
    name = _trans("English");
    break;
  case Language::German:
    name = _trans("German");
    break;
  case Language::French:
    name = _trans("French");
    break;
  case Language::Spanish:
    name = _trans("Spanish");
    break;
  case Language::Italian:
    name = _trans("Italian");
    break;
  case Language::Dutch:
    name = _trans("Dutch");
    break;
  case Language::SimplifiedChinese:
    name = _trans("Simplified Chinese");
    break;
  case Language::TraditionalChinese:
    name = _trans("Traditional Chinese");
    break;
  case Language::Korean:
    name = _trans("Korean");
    break;
  default:
    name = _trans("Unknown");
    break;
  }

  return translate ? Common::GetStringT(name.c_str()) : name;
}

std::string GetName(Region region, bool translate)
{
  std::string name;

  switch (region)
  {
  case Region::NTSC_J:
    name = _trans("NTSC-J");
    break;
  case Region::NTSC_U:
    name = _trans("NTSC-U");
    break;
  case Region::PAL:
    name = _trans("PAL");
    break;
  case Region::NTSC_K:
    name = _trans("NTSC-K");
    break;
  default:
    name = _trans("Unknown");
    break;
  }

  return translate ? Common::GetStringT(name.c_str()) : name;
}

bool IsDisc(Platform volume_type)
{
  return volume_type == Platform::GameCubeDisc || volume_type == Platform::Triforce ||
         volume_type == Platform::WiiDisc;
}

bool IsWii(Platform volume_type)
{
  return volume_type == Platform::WiiDisc || volume_type == Platform::WiiWAD;
}

bool IsNTSC(Region region)
{
  return region == Region::NTSC_J || region == Region::NTSC_U || region == Region::NTSC_K;
}

int ToGameCubeLanguage(Language language)
{
  if (language < Language::English || language > Language::Dutch)
    return 0;
  else
    return static_cast<int>(language) - 1;
}

Language FromGameCubeLanguage(int language)
{
  if (language < 0 || language > 5)
    return Language::Unknown;
  else
    return static_cast<Language>(language + 1);
}

// Increment CACHE_REVISION (GameFileCache.cpp) if the code below is modified

Country TypicalCountryForRegion(Region region)
{
  switch (region)
  {
  case Region::NTSC_J:
    return Country::Japan;
  case Region::NTSC_U:
    return Country::USA;
  case Region::PAL:
    return Country::Europe;
  case Region::NTSC_K:
    return Country::Korea;
  default:
    return Country::Unknown;
  }
}

Region SysConfCountryToRegion(u8 country_code)
{
  if (country_code == 0)
    return Region::Unknown;

  if (country_code < 0x08)  // Japan
    return Region::NTSC_J;

  if (country_code < 0x40)  // Americas
    return Region::NTSC_U;

  if (country_code < 0x80)  // Europe, Oceania, parts of Africa
    return Region::PAL;

  if (country_code < 0xa8)  // Southeast Asia
    return country_code == 0x88 ? Region::NTSC_K : Region::NTSC_J;

  if (country_code < 0xc0)  // Middle East
    return Region::NTSC_U;

  return Region::Unknown;
}

Region CountryCodeToRegion(u8 country_code, Platform platform, Region expected_region,
                           const u16* revision)
{
  switch (country_code)
  {
  case '\2':
    return expected_region;  // Wii Menu (same title ID for all regions)

  case 'J':
    return Region::NTSC_J;

  case 'W':
    if (expected_region == Region::PAL)
      return Region::PAL;  // Only the Nordic version of Ratatouille (Wii)
    else
      return Region::NTSC_J;  // Korean GC games in English or Taiwanese Wii games

  case 'E':
    if (platform != Platform::GameCubeDisc)
      return Region::NTSC_U;  // The most common country code for NTSC-U

    if (revision)
    {
      if (*revision >= 0x30)
        return Region::NTSC_J;  // Korean GC games in English
      else
        return Region::NTSC_U;  // The most common country code for NTSC-U
    }
    else
    {
      if (expected_region == Region::NTSC_J)
        return Region::NTSC_J;  // Korean GC games in English
      else
        return Region::NTSC_U;  // The most common country code for NTSC-U
    }

  case 'B':
  case 'N':
    return Region::NTSC_U;

  case 'X':
  case 'Y':
  case 'Z':
    // Additional language versions, store-exclusive versions, other special versions
    return expected_region == Region::NTSC_U ? Region::NTSC_U : Region::PAL;

  case 'D':
  case 'F':
  case 'H':
  case 'I':
  case 'L':
  case 'M':
  case 'P':
  case 'R':
  case 'S':
  case 'U':
  case 'V':
    return Region::PAL;

  case 'K':
  case 'Q':
  case 'T':
    // All of these country codes are Korean, but the NTSC-K region doesn't exist on GC
    return platform == Platform::GameCubeDisc ? Region::NTSC_J : Region::NTSC_K;

  default:
    return Region::Unknown;
  }
}

Country CountryCodeToCountry(u8 country_code, Platform platform, Region region, const u16* revision)
{
  switch (country_code)
  {
  // Worldwide
  case 'A':
    return Country::World;

  // Mixed regions
  case 'X':
  case 'Y':
  case 'Z':
    // Additional language versions, store-exclusive versions, other special versions
    return region == Region::NTSC_U ? Country::USA : Country::Europe;

  case 'W':
    if (platform == Platform::GameCubeDisc)
      return Country::Korea;  // GC games in English released in Korea
    else if (region == Region::PAL)
      return Country::Europe;  // Only the Nordic version of Ratatouille (Wii)
    else
      return Country::Taiwan;  // Wii games in traditional Chinese released in Taiwan

  // PAL
  case 'D':
    return Country::Germany;

  case 'L':  // NTSC-J games released on PAL VC
  case 'M':  // NTSC-U games released on PAL VC
  case 'V':  // Used by some Nordic Wii releases
  case 'P':  // The most common country code for PAL
    return Country::Europe;

  case 'U':
    return Country::Australia;

  case 'F':
    return Country::France;

  case 'I':
    return Country::Italy;

  case 'H':
    return Country::Netherlands;

  case 'R':
    return Country::Russia;

  case 'S':
    return Country::Spain;

  // NTSC
  case 'E':
    if (platform != Platform::GameCubeDisc)
      return Country::USA;  // The most common country code for NTSC-U

    if (revision)
    {
      if (*revision >= 0x30)
        return Country::Korea;  // GC games in English released in Korea
      else
        return Country::USA;  // The most common country code for NTSC-U
    }
    else
    {
      if (region == Region::NTSC_J)
        return Country::Korea;  // GC games in English released in Korea
      else
        return Country::USA;  // The most common country code for NTSC-U
    }

  case 'B':  // PAL games released on NTSC-U VC
  case 'N':  // NTSC-J games released on NTSC-U VC
    return Country::USA;

  case 'J':
    return Country::Japan;

  case 'K':  // Games in Korean released in Korea
  case 'Q':  // NTSC-J games released on NTSC-K VC
  case 'T':  // NTSC-U games released on NTSC-K VC
    return Country::Korea;

  default:
    if (country_code > 'A')  // Silently ignore IOS wads
      WARN_LOG(DISCIO, "Unknown Country Code! %c", static_cast<char>(country_code));
    return Country::Unknown;
  }
}

Region GetSysMenuRegion(u16 title_version)
{
  switch (title_version & 0xf)
  {
  case 0:
    return Region::NTSC_J;
  case 1:
    return Region::NTSC_U;
  case 2:
    return Region::PAL;
  case 6:
    return Region::NTSC_K;
  default:
    return Region::Unknown;
  }
}

}  // namespace DiscIO
