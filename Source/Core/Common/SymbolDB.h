// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// This file contains a generic symbol map implementation. For CPU-specific
// magic, derive and extend.

#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common
{
struct Symbol
{
  enum class Type
  {
    Function,
    Data,
  };

  Symbol() = default;
  explicit Symbol(const std::string& symbol_name) { Rename(symbol_name); }

  void Rename(const std::string& symbol_name);

  std::string name;
  std::string function_name;  // stripped function name
  u32 address = 0;
  u32 size = 0;
  Type type = Type::Function;
  bool analyzed = false;
};

// Used by the emulation (CPU) thread alone: boot fills it before that thread runs, and every
// later reader and writer (HLE, the JIT, the interpreter, the debugger stub) runs on it.
class SymbolDB
{
public:
  using XFuncMap = std::map<u32, Symbol>;

  SymbolDB();
  ~SymbolDB();

  std::vector<const Symbol*> GetSymbolsFromName(std::string_view name) const;

  bool IsEmpty() const;
  bool Clear();

protected:
  XFuncMap m_functions;
  std::string m_map_name;
};
}  // namespace Common
