// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/GeckoCodeConfig.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "Common/IniData.h"
#include "Common/IniFile.h"
#include "Core/CheatCodes.h"

namespace Gecko
{
/* Frees what the code holds, and leaves it empty. */
static void CodeFree(GeckoCode* code)
{
  u32 l;
  for (l = 0; l < code->num_lines; l++)
    std::free(code->lines[l].original_line);
  std::free(code->lines);
  std::free(code->name);
  std::memset(code, 0, sizeof(*code));
}

void FreeCodeList(GeckoCodeList* list)
{
  u32 c;
  for (c = 0; c < list->count; c++)
    CodeFree(&list->codes[c]);
  std::free(list->codes);
  std::memset(list, 0, sizeof(*list));
}

/* The code goes behind the others in the list, which owns it from here; dropped when out of
 * memory. */
static void CodeKeep(GeckoCodeList* list, GeckoCode* code)
{
  if (list->count == list->cap)
  {
    const u32 cap = list->cap ? list->cap * 2 : 16;
    GeckoCode* const grown =
        static_cast<GeckoCode*>(std::realloc(list->codes, cap * sizeof(GeckoCode)));
    if (!grown)
    {
      CodeFree(code);
      return;
    }
    list->codes = grown;
    list->cap = cap;
  }
  list->codes[list->count++] = *code;
  std::memset(code, 0, sizeof(*code));
}

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

/* A field of a line as a hexadecimal number, the whole field being the number. */
static bool ParseField(const char* field, u32* out)
{
  char* end = nullptr;
  unsigned long long value;
  errno = 0;
  value = std::strtoull(field, &end, 16);
  if (!end || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFull)
    return false;
  *out = static_cast<u32>(value);
  return true;
}

/* A line "address data", separated by spaces: both words, else neither. */
static void DeserializeLine(GeckoCodeLine* line)
{
  const size_t len = std::strlen(line->original_line);
  char* const fields = CopyText(line->original_line, len);
  char* first = nullptr;
  char* second = nullptr;
  u32 num_fields = 0;
  size_t start = 0;
  u32 address;
  u32 data;

  line->address = 0;
  line->data = 0;
  if (!fields)
    return;
  while (start < len)
  {
    char* const space = static_cast<char*>(std::memchr(fields + start, ' ', len - start));
    const size_t end = space ? size_t(space - fields) : len;
    fields[end] = '\0';
    if (num_fields == 0)
      first = fields + start;
    else if (num_fields == 1)
      second = fields + start;
    num_fields++;
    start = end + 1;
  }
  if (num_fields >= 2 && ParseField(first, &address) && ParseField(second, &data))
  {
    line->address = address;
    line->data = data;
  }
  std::free(fields);
}

static void CodeAddLine(GeckoCode* code, const char* text, size_t len)
{
  GeckoCodeLine* line;
  if (code->num_lines == code->cap_lines)
  {
    const u32 cap = code->cap_lines ? code->cap_lines * 2 : 8;
    GeckoCodeLine* const grown =
        static_cast<GeckoCodeLine*>(std::realloc(code->lines, cap * sizeof(GeckoCodeLine)));
    if (!grown)
      return;
    code->lines = grown;
    code->cap_lines = cap;
  }
  line = &code->lines[code->num_lines];
  line->original_line = CopyText(text, len);
  if (!line->original_line)
    return;
  DeserializeLine(line);
  code->num_lines++;
}

static bool IsSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The codes of one INI file, behind those already read. */
static void LoadSection(const Common::IniFile& ini, GeckoCodeList* list)
{
  const Common::IniFile::Section* const lines = ini.GetSection("Gecko");
  GeckoCode code = {};
  size_t i;

  for (i = 0; lines && i < lines->Data()->num_lines; i++)
  {
    const char* text;
    size_t len;
    if (!ini_section_line(lines->Data(), i, false, &text, &len) || !len || text[0] == '#')
      continue;

    switch (text[0])
    {
    // enabled or disabled code: "+$name [creator]" or "$name [creator]"
    case '+':
    case '$':
    {
      const bool enabled = text[0] == '+';
      size_t start = enabled ? 2 : 1;
      const char* open;
      size_t end;
      if (code.name && code.name[0])
        CodeKeep(list, &code);
      CodeFree(&code);
      code.enabled = enabled;
      if (start > len)
        start = len;
      // the code name stops at '[', where the creator's name begins
      open = static_cast<const char*>(std::memchr(text + start, '[', len - start));
      end = open ? size_t(open - text) : len;
      while (start < end && IsSpace(text[start]))
        start++;
      while (end > start && IsSpace(text[end - 1]))
        end--;
      code.name = CopyText(text + start, end - start);
      break;
    }

    // notes, which nothing reads
    case '*':
      break;

    // either part of the code, or an option choice
    default:
      CodeAddLine(&code, text, len);
      break;
    }
  }

  // add the last code
  if (code.name && code.name[0])
    CodeKeep(list, &code);
  CodeFree(&code);

  ReadEnabledAndDisabled(ini, "Gecko", list->codes, list->count, sizeof(GeckoCode),
                         offsetof(GeckoCode, name), offsetof(GeckoCode, enabled));
}

void LoadCodes(const Common::IniFile& globalIni, const Common::IniFile& localIni,
               GeckoCodeList* list)
{
  FreeCodeList(list);
  LoadSection(globalIni, list);
  LoadSection(localIni, list);
}
}  // namespace Gecko
