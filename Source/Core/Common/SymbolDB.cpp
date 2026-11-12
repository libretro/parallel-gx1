// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/SymbolDB.h"

#include <cinttypes>
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

void SymbolDB::List()
{
  for (const auto& func : m_functions)
  {
    DEBUG_LOG(OSHLE, "%s @ %08x: %u bytes (hash %08x) : %d calls", func.second.name.c_str(),
              func.second.address, func.second.size, func.second.hash, func.second.num_calls);
  }
  INFO_LOG(OSHLE, "%" PRIu64 " functions known in this program above.", (u64)m_functions.size());
}

bool SymbolDB::IsEmpty() const
{
  return m_functions.empty() && m_notes.empty();
}

bool SymbolDB::Clear(const char* prefix)
{

  // TODO: honor prefix
  m_map_name.clear();
  if (IsEmpty())
    return false;

  m_functions.clear();
  m_notes.clear();
  m_checksum_to_function.clear();
  return true;
}

void SymbolDB::Index()
{
  Index(&m_functions);
}

void SymbolDB::Index(XFuncMap* functions)
{
  int i = 0;
  for (auto& func : *functions)
  {
    func.second.index = i++;
  }
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

bool SymbolDB::RenameSymbol(const Symbol& symbol, const std::string& symbol_name)
{

  auto it = m_functions.find(symbol.address);
  if (it == m_functions.end())
    return false;

  it->second.Rename(symbol_name);
  return true;
}

bool SymbolDB::RenameSymbol(const Symbol& symbol, const std::string& symbol_name,
                            const std::string& object_name)
{

  auto it = m_functions.find(symbol.address);
  if (it == m_functions.end())
    return false;

  it->second.Rename(symbol_name);
  it->second.object_name = object_name;
  return true;
}

}  // namespace Common
