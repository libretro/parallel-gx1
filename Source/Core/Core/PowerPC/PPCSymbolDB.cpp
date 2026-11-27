// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/PPCSymbolDB.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

PPCSymbolDB::PPCSymbolDB() = default;

PPCSymbolDB::~PPCSymbolDB() = default;

void PPCSymbolDB::AddKnownSymbol(u32 startAddr, u32 size, const char* name,
                                 Common::Symbol::Type type)
{
  AddKnownSymbol(startAddr, size, name, type, &m_functions);
}

void PPCSymbolDB::AddKnownSymbol(u32 startAddr, u32 size, const char* name,
                                 Common::Symbol::Type type, Common::SymbolList* functions)
{
  bool inserted;
  Common::Symbol* const symbol = functions->FindOrInsert(startAddr, name, &inserted);
  if (!symbol)
    return;
  symbol->type = type;

  if (!inserted)
  {
    // already got it, let's just update name & size to be sure.
    symbol->Rename(name);
    symbol->size = size;
  }
  else
  {
    // new symbol. run analyze.
    symbol->address = startAddr;

    if (symbol->type == Common::Symbol::Type::Function)
    {
      PPCAnalyst::AnalyzeFunction(startAddr, *symbol, size);
      // Do not truncate symbol when a size is expected
      if (size != 0 && symbol->size != size)
      {
        WARN_LOG(SYMBOLS, "Analysed symbol (%s) size mismatch, %u expected but %u computed", name,
                 size, symbol->size);
        symbol->size = size;
      }
    }
    else
    {
      symbol->size = size;
    }
  }
}

const Common::Symbol* PPCSymbolDB::GetSymbolFromAddr(u32 addr) const
{
  const u32 index = m_functions.LowerBound(addr);

  // If the address is exactly the start address of a symbol, we're done.
  if (index < m_functions.count && m_functions.symbols[index].address == addr)
    return &m_functions.symbols[index];

  // Otherwise, check whether the address is within the bounds of a symbol.
  if (index > 0)
  {
    const Common::Symbol& symbol = m_functions.symbols[index - 1];
    if (addr >= symbol.address && addr < symbol.address + symbol.size)
      return &symbol;
  }

  return nullptr;
}

// Returns true if m_functions was changed.
bool PPCSymbolDB::LoadMapOnBoot()
{
  // the map file for the active title
  const std::string& maps_directory = File::GetUserPath(D_MAPS_IDX);
  const std::string& game_id = SConfig::GetInstance().m_debugger_game_id;
  const size_t path_size = maps_directory.size() + game_id.size() + sizeof(".map");
  char* const existing_map_file = static_cast<char*>(std::malloc(path_size));
  if (!existing_map_file)
    return Clear();
  snprintf(existing_map_file, path_size, "%s%s.map", maps_directory.c_str(), game_id.c_str());

  bool changed;
  if (!File::Exists(existing_map_file))
    changed = Clear();
  // If the map is already loaded (such as restarting the same game), skip reloading.
  else if (!IsEmpty() && m_map_name && !std::strcmp(existing_map_file, m_map_name))
    changed = false;
  else if (!LoadMap(existing_map_file))
    changed = Clear();
  else
    changed = true;

  std::free(existing_map_file);
  return changed;
}

static bool IsMapWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The text without the whitespace around it, as its start and *length
static const char* TrimMapWhitespace(const char* text, size_t* length)
{
  size_t end = std::strlen(text);
  while (end && IsMapWhitespace(text[end - 1]))
    end--;
  size_t start = 0;
  while (start < end && IsMapWhitespace(text[start]))
    start++;
  *length = end - start;
  return text + start;
}

static bool IsHexWord(const char* word, size_t length)
{
  if (!length)
    return false;
  for (size_t i = 0; i < length; i++)
  {
    const char c = word[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

// Some entries in the table have a function name followed by " (entry of " followed by a
// container name, followed by ")"; the name becomes "<container>::<name>", keeping what
// follows the entry part, usually the object names.
static void ParseEntryOf(char* name_buf)
{
  static const char ENTRY_OF_STRING[] = " (entry of ";
  char* const s1 = std::strstr(name_buf, ENTRY_OF_STRING);
  if (!s1)
    return;

  char container[512];
  char* ptr = s1 + sizeof(ENTRY_OF_STRING) - 1;
  if (sscanf(ptr, "%511s", container) != 1)
    container[0] = '\0';
  // Skip sections, those start with a dot, e.g. (entry of .text)
  char* const s2 = std::strchr(container, ')');
  if (!s2 || *container == '.')
    return;

  ptr += std::strlen(container);
  std::memmove(s1, ptr, std::strlen(ptr) + 1);
  *s2 = '\0';
  // "<container>::<name>", cut to the buffer's 512 octets as the old strcat ran over them
  char joined[512];
  snprintf(joined, sizeof(joined), "%s::%s", container, name_buf);
  std::memcpy(name_buf, joined, std::strlen(joined) + 1);
}

static bool WasAlignment(const char* name_buf)
{
  return *name_buf == ' ' || (*name_buf >= '0' && *name_buf <= '9');
}

static void ParseAlignment(char* name_buf, u32* alignment_buf)
{
  // the name without the whitespace around it, then the alignment and the name after it
  char buffer[512];
  size_t length;
  const char* const trimmed = TrimMapWhitespace(name_buf, &length);
  std::memcpy(buffer, trimmed, length);
  buffer[length] = '\0';
  sscanf(buffer, "%i %511[^\r\n]", alignment_buf, name_buf);
}

// The use case for handling bad map files is when you have a game with a map file on the disc,
// but you can't tell whether that map file is for the particular release version used in that game,
// or when you know that the map file is not for that build, but perhaps half the functions in the
// map file are still at the correct locations. Which are both common situations. It will load any
// function names and addresses that have a BLR before the start and at the end, but ignore any that
// don't, and then tell you how many were good and how many it ignored. That way you either find out
// it is all good and use it, find out it is partly good and use the good part, or find out that
// only a handful of functions lined up by coincidence and then you can clear the symbols. In the
// future I want to make it smarter, so it checks that there are no BLRs in the middle of the
// function (by checking the code length), and also make it cope with added functions in the middle
// or work based on the order of the functions and their approximate length. Currently that process
// has to be done manually and is very tedious.
// The use case for separate handling of map files that aren't bad is that you usually want to also
// load names that aren't functions(if included in the map file) without them being rejected as
// invalid.
// You can see discussion about these kinds of issues here :
// https://forums.oculus.com/viewtopic.php?f=42&t=11241&start=580
// https://m2k2.taigaforum.com/post/metroid_prime_hacking_help_25.html#metroid_prime_hacking_help_25

// This one can load both leftover map files on game discs (like Zelda) and
// CodeWarrior-style map files.
// bad=true means carefully load map files that might not be from exactly the right version
bool PPCSymbolDB::LoadMap(const char* filename, bool bad)
{
  File::LineReader f(filename);
  if (!f.IsOpen())
    return false;

  Common::SymbolList new_functions;

  // Two columns are used by Super Smash Bros. Brawl Korean map file
  // Three columns are commonly used
  // Four columns are used in American Mensa Academy map files and perhaps other games
  int column_count = 0;
  int good_count = 0;
  int bad_count = 0;

  char line[512];
  char section_name[256] = "";
  const char* text;
  size_t text_length;
  while (f.Next(&text, &text_length))
  {
    // as the line a 512-octet fgets() gives
    if (text_length > sizeof(line) - 2)
      text_length = sizeof(line) - 2;
    std::memcpy(line, text, text_length);
    line[text_length] = '\n';
    line[text_length + 1] = '\0';
    size_t length = strlen(line);
    if (length < 4)
      continue;

    char temp[256]{};
    sscanf(line, "%255s", temp);

    if (strcmp(temp, "UNUSED") == 0)
      continue;

    // Support CodeWarrior and Dolphin map
    static const char SECTION_LAYOUT[] = " section layout";
    size_t trimmed_length;
    const char* const trimmed_line = TrimMapWhitespace(line, &trimmed_length);
    if ((trimmed_length >= sizeof(SECTION_LAYOUT) - 1 &&
         !std::memcmp(trimmed_line + trimmed_length - (sizeof(SECTION_LAYOUT) - 1), SECTION_LAYOUT,
                      sizeof(SECTION_LAYOUT) - 1)) ||
        strcmp(temp, ".text") == 0 || strcmp(temp, ".init") == 0)
    {
      std::memcpy(section_name, temp, sizeof(section_name));
      continue;
    }

    // Skip four columns' header.
    //
    // Four columns example:
    //
    // .text section layout
    //   Starting        Virtual
    //   address  Size   address
    //   -----------------------
    if (strcmp(temp, "Starting") == 0)
      continue;
    if (strcmp(temp, "address") == 0)
      continue;
    if (strcmp(temp, "-----------------------") == 0)
      continue;

    // Skip link map.
    //
    // Link map example:
    //
    // Link map of __start
    //  1] __start(func, weak) found in os.a __start.c
    //   2] __init_registers(func, local) found in os.a __start.c
    //    3] _stack_addr found as linker generated symbol
    // ...
    //           10] EXILock(func, global) found in exi.a EXIBios.c
    if (temp[0] && temp[std::strlen(temp) - 1] == ']')
      continue;

    // TODO - Handle/Write a parser for:
    //  - Memory map
    //  - Link map
    //  - Linker generated symbols
    if (!section_name[0])
      continue;

    // Column detection heuristic
    if (column_count == 0)
    {
      // The first four space-separated words of the line
      const char* words[4];
      size_t word_lengths[4];
      size_t word_count = 0;
      {
        size_t i = 0;
        while (word_count < 4 && i < trimmed_length)
        {
          while (i < trimmed_length && (trimmed_line[i] == ' ' || trimmed_line[i] == '\t'))
            i++;
          const size_t start = i;
          while (i < trimmed_length && trimmed_line[i] != ' ' && trimmed_line[i] != '\t')
            i++;
          if (i > start)
          {
            words[word_count] = trimmed_line + start;
            word_lengths[word_count++] = i - start;
          }
        }
      }

      // Two columns format:
      // 80004000 zz_80004000_
      if (word_count < 1 || word_lengths[0] != 8 || !IsHexWord(words[0], word_lengths[0]))
        continue;
      column_count = 2;

      // Three columns format (with optional alignment):
      //  Starting        Virtual
      //  address  Size   address
      //  -----------------------
      if (word_count >= 3 && IsHexWord(words[1], word_lengths[1]) &&
          IsHexWord(words[2], word_lengths[2]))
      {
        column_count = 3;

        // Four columns format (with optional alignment):
        //  Starting        Virtual  File
        //  address  Size   address  offset
        //  ---------------------------------
        if (word_count >= 4 && word_lengths[3] == 8 && IsHexWord(words[3], word_lengths[3]))
          column_count = 4;
      }
    }

    u32 address;
    u32 vaddress;
    u32 size = 0;
    u32 offset = 0;
    u32 alignment = 0;
    char name[512]{};
    switch (column_count)
    {
    case 4:
      // sometimes there is no alignment value, and sometimes it is because it is an entry of
      // something else
      sscanf(line, "%08x %08x %08x %08x %511[^\r\n]", &address, &size, &vaddress, &offset, name);
      if (WasAlignment(name))
        ParseAlignment(name, &alignment);
      // The `else` statement was omitted to handle symbol already saved in Dolphin symbol map
      // since it doesn't omit the alignment on save for such case.
      ParseEntryOf(name);
      break;
    case 3:
      // some entries in the table have a function name followed by " (entry of " followed by a
      // container name, followed by ")"
      // instead of a space followed by a number followed by a space followed by a name
      sscanf(line, "%08x %08x %08x %511[^\r\n]", &address, &size, &vaddress, name);
      if (WasAlignment(name))
        ParseAlignment(name, &alignment);
      // The `else` statement was omitted to handle symbol already saved in Dolphin symbol map
      // since it doesn't omit the alignment on save for such case.
      ParseEntryOf(name);
      break;
    case 2:
      sscanf(line, "%08x %511[^\r\n]", &address, name);
      vaddress = address;
      break;
    default:
      // Should never happen
      break;
    }

    // Check if this is a valid entry.
    if (name[0])
    {
      // The name, before any tab and the object name after it, without the whitespace around it
      char* const tab = std::strchr(name, '\t');
      if (tab)
        *tab = '\0';
      size_t name_length;
      char* const name_string = const_cast<char*>(TrimMapWhitespace(name, &name_length));
      name_string[name_length] = '\0';

      bool good;
      // Notes will be treated the same as Data.
      const Common::Symbol::Type type = !strcmp(section_name, ".text") || !strcmp(section_name, ".init") ?
                                            Common::Symbol::Type::Function :
                                            Common::Symbol::Type::Data;

      if (type == Common::Symbol::Type::Function)
      {
        // Can't compute the checksum if not in RAM
        good = !bad && PowerPC::MMU::HostIsInstructionRAMAddress(vaddress) &&
               PowerPC::MMU::HostIsInstructionRAMAddress(vaddress + size - 4);
        if (!good)
        {
          // check for BLR before function
          PowerPC::TryReadInstResult read_result =
              Core::System::GetInstance().GetMMU().TryReadInstruction(vaddress - 4);
          if (read_result.valid && read_result.hex == 0x4e800020)
          {
            // check for BLR at end of function
            read_result =
                Core::System::GetInstance().GetMMU().TryReadInstruction(vaddress + size - 4);
            good = read_result.valid && read_result.hex == 0x4e800020;
          }
        }
      }
      else
      {
        // Data type, can have any length.
        good = !bad && PowerPC::MMU::HostIsRAMAddress(vaddress) &&
               PowerPC::MMU::HostIsRAMAddress(vaddress + size - 1);
      }

      if (good)
      {
        ++good_count;

        // notes, which the debugger drew, are not kept
        if (strcmp(section_name, ".note") != 0)
          AddKnownSymbol(vaddress, size, name_string, type, &new_functions);
      }
      else
      {
        ++bad_count;
      }
    }
  }

  m_functions.Free();
  m_functions = new_functions;
  const size_t filename_size = std::strlen(filename) + 1;
  char* const map_name = static_cast<char*>(std::malloc(filename_size));
  if (map_name)
    std::memcpy(map_name, filename, filename_size);
  std::free(m_map_name);
  m_map_name = map_name;

  NOTICE_LOG(SYMBOLS, "%d symbols loaded, %d symbols ignored.", good_count, bad_count);
  return true;
}
