// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/IniFile.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "Common/StringUtil.h"

namespace Common
{
const std::string& IniFile::NULL_STRING = "";

void IniFile::Section::Set(std::string_view key, std::string new_value)
{
  ini_section_set(this, key.data(), key.size(), new_value.data(), new_value.size());
}

bool IniFile::Section::Get(std::string_view key, std::string* value,
                           const std::string& default_value) const
{
  const char* const found = ini_section_get(this, key.data(), key.size());

  if (found)
  {
    *value = found;
    return true;
  }

  if (&default_value != &NULL_STRING)
  {
    *value = default_value;
    return true;
  }

  return false;
}

bool IniFile::Section::Exists(std::string_view key) const
{
  return ini_section_get(this, key.data(), key.size()) != nullptr;
}

bool IniFile::Section::Delete(std::string_view key)
{
  return ini_section_delete(this, key.data(), key.size());
}

// IniFile

IniFile::IniFile()
{
  ini_data_init(&m_data);
}

IniFile::~IniFile()
{
  ini_data_free(&m_data);
}

IniFile::IniFile(IniFile&& other) : m_data(other.m_data)
{
  ini_data_init(&other.m_data);
}

IniFile& IniFile::operator=(IniFile&& other)
{
  if (this != &other)
  {
    ini_data_free(&m_data);
    m_data = other.m_data;
    ini_data_init(&other.m_data);
  }
  return *this;
}

const IniFile::Section* IniFile::GetSection(std::string_view section_name) const
{
  return static_cast<const Section*>(
      ini_data_section(&m_data, section_name.data(), section_name.size()));
}

IniFile::Section* IniFile::GetSection(std::string_view section_name)
{
  return static_cast<Section*>(ini_data_section(&m_data, section_name.data(), section_name.size()));
}

IniFile::Section* IniFile::GetOrCreateSection(std::string_view section_name)
{
  return static_cast<Section*>(
      ini_data_section_make(&m_data, section_name.data(), section_name.size()));
}

bool IniFile::Exists(std::string_view section_name) const
{
  return GetSection(section_name) != nullptr;
}

bool IniFile::Exists(std::string_view section_name, std::string_view key) const
{
  const Section* section = GetSection(section_name);
  if (!section)
    return false;

  return section->Exists(key);
}

bool IniFile::DeleteKey(std::string_view section_name, std::string_view key)
{
  Section* section = GetSection(section_name);
  if (!section)
    return false;
  return section->Delete(key);
}

bool IniFile::Load(const std::string& filename, bool keep_current_data)
{
  return ini_data_load(&m_data, filename.c_str(), keep_current_data);
}

bool IniFile::Save(const std::string& filename)
{
  return ini_data_save(&m_data, filename.c_str());
}
}  // namespace Common
