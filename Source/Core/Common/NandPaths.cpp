// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/NandPaths.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace Common
{
std::string RootUserPath(FromWhichRoot from)
{
  int idx{};
  switch (from)
  {
  case FromWhichRoot::None:
    return "";
  case FromWhichRoot::Configured:
    idx = D_WIIROOT_IDX;
    break;
  case FromWhichRoot::Session:
    idx = D_SESSION_WIIROOT_IDX;
    break;
  case FromWhichRoot::Banners:
    idx = D_BANNERS_WIIROOT_IDX;
    break;
  }
  std::string dir = File::GetUserPath(idx);
  dir.pop_back();  // remove trailing path separator
  return dir;
}

/* root + dir + "%08x/%08x" of the title ID + suffix */
static std::string TitleIDPath(FromWhichRoot from, const char* dir, u64 title_id,
                               const char* suffix)
{
  char id[20];
  snprintf(id, sizeof(id), "%08x/%08x", static_cast<u32>(title_id >> 32),
           static_cast<u32>(title_id));
  std::string path = RootUserPath(from);
  path += dir;
  path += id;
  path += suffix;
  return path;
}

std::string GetImportTitlePath(u64 title_id, FromWhichRoot from)
{
  return TitleIDPath(from, "/import/", title_id, "");
}

std::string GetTicketFileName(u64 title_id, FromWhichRoot from)
{
  return TitleIDPath(from, "/ticket/", title_id, ".tik");
}

std::string GetV1TicketFileName(u64 title_id, FromWhichRoot from)
{
  return TitleIDPath(from, "/ticket/", title_id, ".tv1");
}

std::string GetTitlePath(u64 title_id, FromWhichRoot from)
{
  return TitleIDPath(from, "/title/", title_id, "");
}

std::string GetTitleDataPath(u64 title_id, FromWhichRoot from)
{
  return GetTitlePath(title_id, from) + "/data";
}

std::string GetTitleContentPath(u64 title_id, FromWhichRoot from)
{
  return GetTitlePath(title_id, from) + "/content";
}

std::string GetTMDFileName(u64 title_id, FromWhichRoot from)
{
  return GetTitleContentPath(title_id, from) + "/title.tmd";
}

std::string GetMiiDatabasePath(FromWhichRoot from)
{
  return RootUserPath(from) + "/shared2/menu/FaceLib/RFL_DB.dat";
}

static bool IsIllegalCharacter(char c)
{
  static constexpr char illegal_chars[] = {'\"', '*', '/', ':', '<', '>', '?', '\\', '|', '\x7f'};
  if (static_cast<unsigned char>(c) <= 0x1F)
    return true;
  for (const char illegal : illegal_chars)
  {
    if (c == illegal)
      return true;
  }
  return false;
}

std::string EscapeFileName(const std::string& filename)
{
  // Prevent paths from containing special names like ., .., ..., ...., and so on
  if (filename.find_first_not_of('.') == std::string::npos)
    return ReplaceAll(filename, ".", "__2e__");

  // Escape all double underscores since we will use double underscores for our escape sequences
  std::string filename_with_escaped_double_underscores = ReplaceAll(filename, "__", "__5f____5f__");

  // Escape all other characters that need to be escaped
  std::string result;
  result.reserve(filename_with_escaped_double_underscores.size());
  for (char c : filename_with_escaped_double_underscores)
  {
    if (IsIllegalCharacter(c))
    {
      char escaped[8];
      snprintf(escaped, sizeof(escaped), "__%02x__", static_cast<unsigned char>(c));
      result.append(escaped);
    }
    else
      result.push_back(c);
  }

  return result;
}

std::string EscapePath(const std::string& path)
{
  const std::vector<std::string> split_strings = SplitString(path, '/');

  std::string escaped;
  for (size_t i = 0; i < split_strings.size(); i++)
  {
    if (i)
      escaped += '/';
    escaped += EscapeFileName(split_strings[i]);
  }
  return escaped;
}

// Appends size bytes of text to out at *length, as long as they fit with a terminator after them.
static bool AppendEscaped(char* out, size_t out_size, size_t* length, const char* text, size_t size)
{
  if (size >= out_size - *length)
    return false;
  memcpy(out + *length, text, size);
  *length += size;
  return true;
}

// EscapeFileName on the size bytes at name, appended to out.
static bool AppendEscapedFileName(char* out, size_t out_size, size_t* length, const char* name,
                                  size_t size)
{
  size_t i;
  bool only_dots = true;
  for (i = 0; i < size; i++)
  {
    if (name[i] != '.')
    {
      only_dots = false;
      break;
    }
  }

  // Prevent paths from containing special names like ., .., ..., ...., and so on
  if (only_dots)
  {
    for (i = 0; i < size; i++)
    {
      if (!AppendEscaped(out, out_size, length, "__2e__", 6))
        return false;
    }
    return true;
  }

  // Double underscores are escaped first, since escape sequences use them; the escape of a double
  // underscore holds no character that needs escaping itself.
  for (i = 0; i < size; i++)
  {
    const char c = name[i];
    if (c == '_' && i + 1 < size && name[i + 1] == '_')
    {
      if (!AppendEscaped(out, out_size, length, "__5f____5f__", 12))
        return false;
      i++;
    }
    else if (IsIllegalCharacter(c))
    {
      char escaped[8];
      snprintf(escaped, sizeof(escaped), "__%02x__", static_cast<unsigned char>(c));
      if (!AppendEscaped(out, out_size, length, escaped, 6))
        return false;
    }
    else if (!AppendEscaped(out, out_size, length, &c, 1))
    {
      return false;
    }
  }
  return true;
}

bool EscapeFileNameTo(const char* name, size_t size, char* out, size_t out_size)
{
  size_t length = 0;
  bool fits;
  if (out_size == 0)
    return false;
  fits = AppendEscapedFileName(out, out_size, &length, name, size);
  // An append that does not fit writes nothing, so what is there is whole and leaves room.
  out[length] = '\0';
  return fits;
}

bool EscapePathTo(const char* path, char* out, size_t out_size)
{
  // As EscapePath: the path's '/'-separated parts escaped and joined by '/', where a part ends at
  // each '/' and the last ends with the path, so a trailing '/' leaves no empty part after it.
  const size_t path_length = strlen(path);
  size_t length = 0;
  size_t start = 0;
  bool first = true;

  if (out_size == 0)
    return false;
  while (start < path_length)
  {
    const char* const slash =
        static_cast<const char*>(memchr(path + start, '/', path_length - start));
    const size_t end = slash ? static_cast<size_t>(slash - path) : path_length;
    if (!first && !AppendEscaped(out, out_size, &length, "/", 1))
      return false;
    first = false;
    if (!AppendEscapedFileName(out, out_size, &length, path + start, end - start))
      return false;
    start = end + 1;
  }
  out[length] = '\0';
  return true;
}

static int HexDigitValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

size_t UnescapeFileName(char* name, size_t length)
{
  size_t pos;

  // Replace escape sequences of the format "__3f__" with the ASCII character defined by the
  // escape sequence's hex digits. A single leading hex digit is enough, as it always was.
  for (pos = 0; pos + 1 < length; pos++)
  {
    int high, low;
    if (name[pos] != '_' || name[pos + 1] != '_')
      continue;
    if (pos + 6 > length || name[pos + 4] != '_' || name[pos + 5] != '_')
      continue;
    high = HexDigitValue(name[pos + 2]);
    if (high < 0)
      continue;
    low = HexDigitValue(name[pos + 3]);
    name[pos] = static_cast<char>(low < 0 ? high : high * 16 + low);
    std::memmove(name + pos + 1, name + pos + 6, length - pos - 6);
    length -= 5;
  }
  name[length] = '\0';
  return length;
}

}  // namespace Common
