// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/IniData.h"
#include "Common/StringUtil.h"

namespace Common
{
// The sections of an INI file, held as ini_data: this gives its sections and keys the strings and
// typed values the callers pass.
class IniFile
{
public:
  // A section: an ini_section. Those of a file are the file's; one made here is empty, of no
  // file, and frees what it is given.
  class Section : private ini_section
  {
    friend class IniFile;

  public:
    Section() { ini_section_init(this); }
    ~Section() { ini_section_clear(this); }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    bool Exists(std::string_view key) const;
    bool Delete(std::string_view key);

    void Set(const std::string& key, std::string new_value);

    template <typename T>
    void Set(const std::string& key, T&& new_value)
    {
      Set(key, ValueToString(std::forward<T>(new_value)));
    }

    template <typename T>
    void Set(const std::string& key, T&& new_value, const std::common_type_t<T>& default_value)
    {
      if (new_value != default_value)
        Set(key, std::forward<T>(new_value));
      else
        Delete(key);
    }

    bool Get(std::string_view key, std::string* value,
             const std::string& default_value = NULL_STRING) const;

    template <typename T>
    bool Get(std::string_view key, T* value, const std::common_type_t<T>& default_value = {}) const
    {
      std::string temp;
      bool retval = Get(key, &temp);
      if (retval && TryParse(temp, value))
        return true;
      *value = default_value;
      return false;
    }

    bool GetLines(std::vector<std::string>* lines, const bool remove_comments = true) const;

    const char* GetName() const { return name; }

    // The keys, in the order they were first set
    size_t NumKeys() const { return num_keys; }
    const char* KeyName(size_t index) const { return keys[index].name; }
    const char* KeyValue(size_t index) const { return keys[index].value; }
  };

  IniFile();
  ~IniFile();
  IniFile(IniFile&& other);
  IniFile& operator=(IniFile&& other);
  IniFile(const IniFile&) = delete;
  IniFile& operator=(const IniFile&) = delete;

  /**
   * Loads sections and keys.
   * @param filename filename of the ini file which should be loaded
   * @param keep_current_data If true, "extends" the currently loaded list of sections and keys with
   * the loaded data (and replaces existing entries). If false, existing data will be erased.
   * @warning Using any other operations than "Get*" and "Exists" is untested and will behave
   * unexpectedly
   * @todo This really is just a hack to support having two levels of gameinis (defaults and
   * user-specified) and should eventually be replaced with a less stupid system.
   */
  bool Load(const std::string& filename, bool keep_current_data = false);

  bool Save(const std::string& filename);

  bool Exists(std::string_view section_name) const;
  // Returns true if key exists in section
  bool Exists(std::string_view section_name, std::string_view key) const;

  bool GetLines(std::string_view section_name, std::vector<std::string>* lines,
                bool remove_comments = true) const;

  bool DeleteKey(std::string_view section_name, std::string_view key);

  Section* GetOrCreateSection(std::string_view section_name);
  const Section* GetSection(std::string_view section_name) const;
  Section* GetSection(std::string_view section_name);

  // The sections, in the order they were first met
  size_t NumSections() const { return m_data.num_sections; }
  const Section& SectionAt(size_t index) const
  {
    return *static_cast<const Section*>(m_data.sections[index]);
  }

private:
  ini_data m_data;

  static const std::string& NULL_STRING;
};
}  // namespace Common
