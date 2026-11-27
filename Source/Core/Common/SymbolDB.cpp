// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/SymbolDB.h"

#include <cstdlib>
#include <cstring>

namespace Common
{
void Symbol::Rename(const char* symbol_name)
{
  const size_t name_size = std::strlen(symbol_name) + 1;
  char* const copy = static_cast<char*>(std::malloc(name_size));
  if (!copy)
    return;
  std::memcpy(copy, symbol_name, name_size);
  std::free(name);
  name = copy;
  // the name before any argument list, and before any space
  function_name_length = static_cast<u32>(std::strcspn(name, "( "));
}

bool Symbol::HasFunctionName(const char* function_name, size_t length) const
{
  return name && function_name_length == length && !std::memcmp(name, function_name, length);
}

u32 SymbolList::LowerBound(u32 address) const
{
  u32 low = 0;
  u32 high = count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (symbols[mid].address < address)
      low = mid + 1;
    else
      high = mid;
  }
  return low;
}

Symbol* SymbolList::FindOrInsert(u32 address, const char* name, bool* inserted)
{
  const u32 index = LowerBound(address);
  *inserted = false;
  if (index < count && symbols[index].address == address)
    return &symbols[index];

  if (count == capacity)
  {
    const u32 new_capacity = capacity ? capacity * 2 : 256;
    Symbol* const grown =
        static_cast<Symbol*>(std::realloc(symbols, new_capacity * sizeof(Symbol)));
    if (!grown)
      return nullptr;
    symbols = grown;
    capacity = new_capacity;
  }

  Symbol symbol;
  std::memset(&symbol, 0, sizeof(symbol));
  symbol.type = Symbol::Type::Function;
  symbol.Rename(name);
  if (!symbol.name)
    return nullptr;

  std::memmove(&symbols[index + 1], &symbols[index], (count - index) * sizeof(Symbol));
  symbols[index] = symbol;
  count++;
  *inserted = true;
  return &symbols[index];
}

void SymbolList::Free()
{
  for (u32 i = 0; i < count; i++)
    std::free(symbols[i].name);
  std::free(symbols);
  symbols = nullptr;
  count = 0;
  capacity = 0;
}

SymbolDB::SymbolDB() = default;

SymbolDB::~SymbolDB()
{
  m_functions.Free();
  std::free(m_map_name);
}

bool SymbolDB::IsEmpty() const
{
  return m_functions.count == 0;
}

bool SymbolDB::Clear()
{
  std::free(m_map_name);
  m_map_name = nullptr;
  if (IsEmpty())
    return false;

  m_functions.Free();
  return true;
}

const Symbol* SymbolDB::FindFunctionName(const char* function_name, size_t length, u32 start,
                                         u32* found_index) const
{
  for (u32 i = start; i < m_functions.count; i++)
  {
    if (m_functions.symbols[i].HasFunctionName(function_name, length))
    {
      *found_index = i;
      return &m_functions.symbols[i];
    }
  }
  return nullptr;
}

}  // namespace Common
