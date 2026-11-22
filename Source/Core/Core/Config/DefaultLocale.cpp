// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/DefaultLocale.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"
#include "Core/Host.h"
#include "DiscIO/Enums.h"

namespace Config
{
static std::vector<std::string> GetPreferredLocales()
{
  static const std::vector<std::string> locales = Host_GetPreferredLocales();
  return locales;
}

static bool LocaleHas(const std::vector<std::string>& split_locale, const char* part)
{
  for (const std::string& s : split_locale)
  {
    if (s == part)
      return true;
  }
  return false;
}

static bool TryParseLanguage(const std::string& locale, DiscIO::Language* language)
{
  const std::vector<std::string> split_locale = SplitString(locale, '-');
  if (split_locale.empty())
    return false;

  // Special handling of Chinese due to its two writing systems
  if (split_locale[0] == "zh")
  {
    if (LocaleHas(split_locale, "Hans"))
      *language = DiscIO::Language::SimplifiedChinese;
    else if (LocaleHas(split_locale, "Hant"))
      *language = DiscIO::Language::TraditionalChinese;
    // Mainland China and Singapore use simplified characters
    else if (LocaleHas(split_locale, "CN") || LocaleHas(split_locale, "SG"))
      *language = DiscIO::Language::SimplifiedChinese;
    else
      *language = DiscIO::Language::TraditionalChinese;
    return true;
  }

  // Same order as in Wii SYSCONF
  constexpr std::array<std::string_view, 10> LANGUAGES = {
      "ja", "en", "de", "fr", "es", "it", "nl", "zh", "zh", "ko",
  };

  const auto it = std::ranges::find(LANGUAGES, split_locale[0]);
  if (it == LANGUAGES.cend())
    return false;

  *language = static_cast<DiscIO::Language>(it - LANGUAGES.cbegin());
  return true;
}

static DiscIO::Language ComputeDefaultLanguage()
{
  for (const std::string& locale : GetPreferredLocales())
  {
    DiscIO::Language language;
    if (TryParseLanguage(locale, &language))
      return language;
  }

  return DiscIO::Language::English;
}

static bool TryParseCountryCode(const std::string& locale, std::string* country_code)
{
  for (const std::string& part : SplitString(locale, '-'))
  {
    if (part.size() == 2 && Common::IsUpper(part[0]) && Common::IsUpper(part[1]))
    {
      *country_code = part;
      return true;
    }
  }

  return false;
}

static std::string ComputeDefaultCountryCode()
{
#ifdef _WIN32
  // Windows codepath: Check the regional information.
  // More likely to match the user's physical location than locales are.
  const int buffer_size = GetUserDefaultGeoName(nullptr, 0);
  if (buffer_size == 3)
  {
    std::wstring buffer(buffer_size, L'\0');
    const int result = GetUserDefaultGeoName(buffer.data(), buffer_size);
    buffer.resize(2);
    if (result > 0)
      return WStringToUTF8(buffer);
  }
#endif

  // Generic codepath: Check the locales.
  // Might be entirely unrelated to someone's actual country (for instance someone in a
  // non-English-speaking country using en-US), but could also be a perfect match.
  for (const std::string& locale : GetPreferredLocales())
  {
    std::string country_code;
    if (TryParseCountryCode(locale, &country_code))
      return country_code;
  }

  return "";
}

// False when the host gives no country the Wii knows
static bool ComputeDefaultCountry(u8* country_out)
{
  // clang-format off
  // Same order as in Wii SYSCONF
  static constexpr std::array<std::string_view, 178> COUNTRIES = {
    "--", "JP", "--", "--", "--", "--", "--", "--", "AI", "AG", "AR", "AW", "BS", "BB", "BZ", "BO",
    "BR", "VG", "CA", "KY", "CL", "CO", "CR", "DM", "DO", "EC", "SV", "GF", "GD", "GP", "GT", "GY",
    "HT", "HN", "JM", "MQ", "MX", "MS", "AN", "NI", "PA", "PY", "PE", "KN", "LC", "VC", "SR", "TT",
    "TC", "US", "UY", "VI", "VE", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--",
    "AL", "AU", "AT", "BE", "BA", "BW", "BG", "HR", "CY", "CZ", "DK", "EE", "FI", "FR", "DE", "GR",
    "HU", "IS", "IE", "IT", "LV", "LS", "LI", "LT", "LU", "MK", "MT", "ME", "MZ", "NA", "NL", "NZ",
    "NO", "PL", "PT", "RO", "RU", "RS", "SK", "SI", "ZA", "ES", "SZ", "SE", "CH", "TR", "GB", "ZM",
    "ZW", "AZ", "MR", "ML", "NE", "TD", "SD", "ER", "DJ", "SO", "--", "--", "--", "--", "--", "--",
    "TW", "--", "--", "--", "--", "--", "--", "--", "KR", "--", "--", "--", "--", "--", "--", "--",
    "HK", "MO", "--", "--", "--", "--", "--", "--", "ID", "SG", "TH", "PH", "MY", "--", "--", "--",
    "CN", "--", "--", "--", "--", "--", "--", "--", "AE", "IN", "EG", "OM", "QA", "KW", "SA", "SY",
    "BH", "JO",
  };
  // clang-format on

  std::string country = ComputeDefaultCountryCode();

  // Netherlands Antilles was split into three new codes after the release of the Wii
  if (country == "BQ" || country == "CW" || country == "SX")
    country = "AN";

  const auto it = std::ranges::find(COUNTRIES, country);
  if (it == COUNTRIES.cend())
    return false;

  *country_out = static_cast<u8>(it - COUNTRIES.cbegin());
  return true;
}

// From the default country, which always has a value (the fallback when the host gives none)
static DiscIO::Region ComputeDefaultRegion()
{
  const DiscIO::Region region = DiscIO::SysConfCountryToRegion(GetDefaultCountry());
  ASSERT(region != DiscIO::Region::Unknown);
  return region;
}

DiscIO::Language GetDefaultLanguage()
{
  static const DiscIO::Language language = ComputeDefaultLanguage();
  return language;
}

static u8 ComputeDefaultCountryOrFallback()
{
  constexpr u8 FALLBACK_COUNTRY = 0x6c;  // Switzerland, as per Dolphin tradition
  u8 country;
  return ComputeDefaultCountry(&country) ? country : FALLBACK_COUNTRY;
}

u8 GetDefaultCountry()
{
  static const u8 country = ComputeDefaultCountryOrFallback();
  return country;
}

DiscIO::Region GetDefaultRegion()
{
  static const DiscIO::Region region = ComputeDefaultRegion();
  return region;
}

}  // namespace Config
