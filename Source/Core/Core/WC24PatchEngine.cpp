// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// WC24PatchEngine
// Allows for replacing URLs used in WC24 requests

#include "Core/WC24PatchEngine.h"

#include <array>
#include <cerrno>
#include <clocale>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "Common/IniData.h"
#include "Common/IniFile.h"

#include "Core/CheatCodes.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

namespace WC24PatchEngine
{
static constexpr std::array<u64, 15> s_wc24_channels{
    Titles::NINTENDO_CHANNEL_NTSC_U,
    Titles::NINTENDO_CHANNEL_NTSC_J,
    Titles::NINTENDO_CHANNEL_PAL,
    Titles::FORECAST_CHANNEL_NTSC_U,
    Titles::FORECAST_CHANNEL_NTSC_J,
    Titles::FORECAST_CHANNEL_PAL,
    Titles::NEWS_CHANNEL_NTSC_U,
    Titles::NEWS_CHANNEL_NTSC_J,
    Titles::NEWS_CHANNEL_PAL,
    Titles::EVERYBODY_VOTES_CHANNEL_NTSC_U,
    Titles::EVERYBODY_VOTES_CHANNEL_NTSC_J,
    Titles::EVERYBODY_VOTES_CHANNEL_PAL,
    Titles::REGION_SELECT_CHANNEL_NTSC_U,
    Titles::REGION_SELECT_CHANNEL_NTSC_J,
    Titles::REGION_SELECT_CHANNEL_PAL,
};

static NetworkPatch* s_patches;
static size_t s_num_patches;
static size_t s_cap_patches;

static char* CopyText(const char* text, size_t len)
{
  char* const copy = static_cast<char*>(std::malloc(len + 1));
  if (copy)
  {
    std::memcpy(copy, text, len);
    copy[len] = '\0';
  }
  return copy;
}

static void FreePatches()
{
  size_t i;
  for (i = 0; i < s_num_patches; i++)
  {
    std::free(s_patches[i].name);
    std::free(s_patches[i].source);
    std::free(s_patches[i].replacement);
  }
  std::free(s_patches);
  s_patches = nullptr;
  s_num_patches = 0;
  s_cap_patches = 0;
}

static bool EqualsNoCase(const char* text, size_t len, const char* word)
{
  size_t i;
  for (i = 0; i < len; i++)
  {
    char c = text[i];
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
    if (!word[i] || c != word[i])
      return false;
  }
  return word[len] == '\0';
}

/* A field as a boolean: true or false whatever the case, or a decimal number that is 1 or 0, with
 * '.' or ',' as its decimal separator. */
static bool ParseBool(const char* text, size_t len, bool* out)
{
  char buf[64];
  const char point = *localeconv()->decimal_point;
  bool is_number = false;
  float value = 0;
  size_t i;

  if (len && len < sizeof(buf))
  {
    char* end = nullptr;
    for (i = 0; i < len; i++)
      buf[i] = (text[i] == '.' || text[i] == ',') ? point : text[i];
    buf[len] = '\0';
    errno = 0;
    value = std::strtof(buf, &end);
    is_number = end != buf && *end == '\0' && errno != ERANGE;
  }
  if ((is_number && value == 1) || EqualsNoCase(text, len, "true"))
    *out = true;
  else if ((is_number && value == 0) || EqualsNoCase(text, len, "false"))
    *out = false;
  else
    return false;
  return true;
}

/* A line "source:replacement:is_kd" into the patch, which keeps its name. */
static bool DeserializeLine(const char* text, size_t len, NetworkPatch* patch)
{
  const char* fields[3] = {};
  size_t lengths[3] = {};
  size_t count = 0;
  size_t start = 0;
  bool is_kd;

  while (start < len)
  {
    const char* const colon = static_cast<const char*>(std::memchr(text + start, ':', len - start));
    const size_t end = colon ? size_t(colon - text) : len;
    if (count < 3)
    {
      fields[count] = text + start;
      lengths[count] = end - start;
    }
    count++;
    start = end + 1;
  }

  if (count != 3 || !ParseBool(fields[2], lengths[2], &is_kd))
    return false;

  std::free(patch->source);
  std::free(patch->replacement);
  patch->source = CopyText(fields[0], lengths[0]);
  patch->replacement = CopyText(fields[1], lengths[1]);
  patch->is_kd = is_kd;
  return patch->source && patch->replacement;
}

/* A copy of the patch goes behind the others; dropped when out of memory. */
static void PatchKeepCopy(const NetworkPatch* patch)
{
  NetworkPatch copy = {};
  if (s_num_patches == s_cap_patches)
  {
    const size_t cap = s_cap_patches ? s_cap_patches * 2 : 16;
    NetworkPatch* const grown =
        static_cast<NetworkPatch*>(std::realloc(s_patches, cap * sizeof(NetworkPatch)));
    if (!grown)
      return;
    s_patches = grown;
    s_cap_patches = cap;
  }
  copy.name = CopyText(patch->name ? patch->name : "", patch->name ? std::strlen(patch->name) : 0);
  copy.source = CopyText(patch->source, std::strlen(patch->source));
  copy.replacement = CopyText(patch->replacement, std::strlen(patch->replacement));
  copy.enabled = patch->enabled;
  copy.is_kd = patch->is_kd;
  if (!copy.name || !copy.source || !copy.replacement)
  {
    std::free(copy.name);
    std::free(copy.source);
    std::free(copy.replacement);
    return;
  }
  s_patches[s_num_patches++] = copy;
}

static void LoadPatchSection(const Common::IniFile& ini)
{
  const Common::IniFile::Section* const lines = ini.GetSection("WC24Patch");
  NetworkPatch patch = {};
  size_t i;

  for (i = 0; lines && i < lines->Data()->num_lines; i++)
  {
    const char* text;
    size_t len;
    if (!ini_section_line(lines->Data(), i, true, &text, &len) || !len)
      continue;

    if (text[0] == '$')
    {
      std::free(patch.name);
      patch.name = CopyText(text + 1, len - 1);
    }
    else if (DeserializeLine(text, len, &patch))
    {
      PatchKeepCopy(&patch);
    }
  }
  std::free(patch.name);
  std::free(patch.source);
  std::free(patch.replacement);

  ReadEnabledAndDisabled(ini, "WC24Patch", s_patches, s_num_patches, sizeof(NetworkPatch),
                         offsetof(NetworkPatch, name), offsetof(NetworkPatch, enabled));
}

static bool IsWC24Channel()
{
  const auto& sconfig = SConfig::GetInstance();
  const u64 title_id = sconfig.GetTitleID();
  for (const u64 channel : s_wc24_channels)
  {
    if (channel == title_id)
      return true;
  }
  return false;
}

static void LoadPatches()
{
  const auto& sconfig = SConfig::GetInstance();
  // We can only load WC24 Channels.
  if (!IsWC24Channel())
    return;

  Common::IniFile ini;
  // If WiiLink is enabled then we load the Default Ini as that has the WiiLink URLs.
  if (Config::Get(Config::MAIN_WII_WIILINK_ENABLE))
    ini = sconfig.LoadDefaultGameIni();
  else
    ini = sconfig.LoadLocalGameIni();

  LoadPatchSection(ini);
}

void Reload()
{
  FreePatches();
  LoadPatches();
}

const char* GetNetworkPatch(const char* source, size_t source_len, bool is_kd)
{
  size_t i;
  for (i = 0; i < s_num_patches; i++)
  {
    const NetworkPatch* const patch = &s_patches[i];
    if (patch->enabled && patch->is_kd == is_kd && std::strlen(patch->source) == source_len &&
        !std::memcmp(patch->source, source, source_len))
    {
      return patch->replacement;
    }
  }
  return nullptr;
}

char* GetNetworkPatchByPayload(const char* payload, size_t size, size_t* patched_size)
{
  size_t pos = 0;
  while (pos < size)
  {
    const char* line_end = nullptr;
    size_t end_of_line = size;
    size_t at;
    for (at = pos; at + 1 < size; at++)
    {
      if (payload[at] == '\r' && payload[at + 1] == '\n')
      {
        line_end = payload + at;
        end_of_line = at;
        break;
      }
    }

    if (size - pos >= 6 && !std::memcmp(payload + pos, "Host: ", 6))
    {
      const char* const domain = payload + pos + 6;
      const size_t domain_len = end_of_line - pos - 6;
      size_t i;
      for (i = 0; i < s_num_patches; i++)
      {
        const NetworkPatch* const patch = &s_patches[i];
        if (!patch->is_kd && patch->enabled && std::strlen(patch->source) == domain_len &&
            !std::memcmp(patch->source, domain, domain_len))
        {
          const size_t replacement_len = std::strlen(patch->replacement);
          const size_t tail = size - end_of_line;
          char* const patched =
              static_cast<char*>(std::malloc(pos + 6 + replacement_len + tail + 1));
          if (!patched)
            return nullptr;
          std::memcpy(patched, payload, pos + 6);
          std::memcpy(patched + pos + 6, patch->replacement, replacement_len);
          std::memcpy(patched + pos + 6 + replacement_len, payload + end_of_line, tail);
          *patched_size = pos + 6 + replacement_len + tail;
          patched[*patched_size] = '\0';
          return patched;
        }
      }

      // No matching patch
      return nullptr;
    }

    if (!line_end)
      break;

    pos = end_of_line + 2;
  }

  return nullptr;
}
}  // namespace WC24PatchEngine
