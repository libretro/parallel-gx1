// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/SymbolDB.h"

#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common
{
static std::string GetStrippedFunctionName(const std::string& symbol_name)
{
  std::string name = symbol_name.substr(0, symbol_name.find('('));
  size_t position = name.find(' ');
  if (position != std::string::npos)
    name.erase(position);
  return name;
}

SymbolDB::SymbolDB() = default;

SymbolDB::~SymbolDB() = default;

void Symbol::Rename(const std::string& symbol_name)
{
  this->name = symbol_name;
  this->function_name = GetStrippedFunctionName(symbol_name);
}

bool SymbolDB::IsEmpty() const
{
  return m_functions.empty();
}

bool SymbolDB::Clear()
{
  m_map_name.clear();
  if (IsEmpty())
    return false;

  m_functions.clear();
  return true;
}

std::vector<const Symbol*> SymbolDB::GetSymbolsFromName(std::string_view name) const
{
  std::vector<const Symbol*> symbols;

  for (auto& func : m_functions)
  {
    if (func.second.function_name == name)
      symbols.push_back(&func.second);
  }

  return symbols;
}

}  // namespace Common
