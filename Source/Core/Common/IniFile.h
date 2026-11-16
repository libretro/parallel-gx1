// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Common/StringUtil.h"

namespace Common
{
class IniFile
{
public:
  class Section
  {
    friend class IniFile;

  public:
    Section();
    explicit Section(std::string name_);
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

    using SectionMap = std::map<std::string, std::string, CaseInsensitiveLess>;

    const std::string& GetName() const { return name; }
    const SectionMap& GetValues() const { return values; }

  protected:
    std::string name;

    std::vector<std::string> keys_order;
    SectionMap values;

    std::vector<std::string> m_lines;
  };

  IniFile();
  ~IniFile();

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

  const std::list<Section>& GetSections() const { return sections; }

private:
  std::list<Section> sections;

  static const std::string& NULL_STRING;
};
}  // namespace Common
