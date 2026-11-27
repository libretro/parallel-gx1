// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// This file contains a generic symbol map implementation. For CPU-specific
// magic, derive and extend.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common
{
// Plain data: the database owns the names and frees them.
struct Symbol
{
  enum class Type
  {
    Function,
    Data,
  };

  // Sets the name to a copy of symbol_name; keeps the old one when out of memory
  void Rename(const char* symbol_name);
  // Whether the stripped function name, the name up to a '(' or ' ', is name's length chars
  bool HasFunctionName(const char* function_name, size_t length) const;

  char* name;
  u32 function_name_length;  // the stripped function name is the start of name
  u32 address;
  u32 size;
  Type type;
  bool analyzed;
};

// Symbols sorted by address, with no two at one address
struct SymbolList
{
  Symbol* symbols = nullptr;
  u32 count = 0;
  u32 capacity = 0;

  // The symbol at address, made with the name, empty but for it, when *inserted; nullptr when
  // out of memory. Pointers into the list last until the next insertion or Free().
  Symbol* FindOrInsert(u32 address, const char* name, bool* inserted);
  // Where address is, or would be inserted
  u32 LowerBound(u32 address) const;
  void Free();
};

// Used by the emulation (CPU) thread alone: boot fills it before that thread runs, and every
// later reader and writer (HLE, the JIT, the interpreter, the debugger stub) runs on it.
class SymbolDB
{
public:
  SymbolDB();
  ~SymbolDB();
  SymbolDB(const SymbolDB&) = delete;
  SymbolDB& operator=(const SymbolDB&) = delete;

  // The first symbol at or after index start whose stripped function name is the length chars
  // of function_name, or nullptr; *found_index tells where it was
  const Symbol* FindFunctionName(const char* function_name, size_t length, u32 start,
                                 u32* found_index) const;

  bool IsEmpty() const;
  bool Clear();

protected:
  SymbolList m_functions;
  char* m_map_name = nullptr;  // owned; the map file the symbols came from
};
}  // namespace Common
