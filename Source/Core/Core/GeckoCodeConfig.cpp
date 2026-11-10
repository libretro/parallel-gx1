// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/GeckoCodeConfig.h"

#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/CheatCodes.h"

namespace Gecko
{
std::vector<GeckoCode> LoadCodes(const Common::IniFile& globalIni, const Common::IniFile& localIni)
{
  std::vector<GeckoCode> gcodes;

  for (const auto* ini : {&globalIni, &localIni})
  {
    std::vector<std::string> lines;
    ini->GetLines("Gecko", &lines, false);

    GeckoCode gcode;

    std::erase_if(lines, [](const auto& line) { return line.empty() || line[0] == '#'; });

    for (auto& line : lines)
    {
      std::istringstream ss(line);

      // Some locales (e.g. fr_FR.UTF-8) don't split the string stream on space
      // Use the C locale to workaround this behavior
      ss.imbue(std::locale::classic());

      switch ((line)[0])
      {
      // enabled or disabled code
      case '+':
        ss.seekg(1);
        [[fallthrough]];
      case '$':
        if (!gcode.name.empty())
          gcodes.push_back(gcode);
        gcode = GeckoCode();
        gcode.enabled = (1 == ss.tellg());  // silly
        gcode.user_defined = (ini == &localIni);
        ss.seekg(1, std::ios_base::cur);
        // read the code name
        std::getline(ss, gcode.name, '[');  // stop at [ character (beginning of contributor name)
        gcode.name = StripWhitespace(gcode.name);
        // read the code creator name
        std::getline(ss, gcode.creator, ']');
        break;

      // notes
      case '*':
        gcode.notes.push_back(std::string(++line.begin(), line.end()));
        break;

      // either part of the code, or an option choice
      default:
      {
        GeckoCode::Code new_code;
        // TODO: support options
        if (std::optional<GeckoCode::Code> code = DeserializeLine(line))
          new_code = *code;
        else
          new_code.original_line = line;
        gcode.codes.push_back(new_code);
      }
      break;
      }
    }

    // add the last code
    if (!gcode.name.empty())
    {
      gcodes.push_back(gcode);
    }

    ReadEnabledAndDisabled(*ini, "Gecko", &gcodes);

    if (ini == &globalIni)
    {
      for (GeckoCode& code : gcodes)
        code.default_enabled = code.enabled;
    }
  }

  return gcodes;
}

std::optional<GeckoCode::Code> DeserializeLine(const std::string& line)
{
  std::vector<std::string> items = SplitString(line, ' ');

  GeckoCode::Code code;
  code.original_line = line;

  if (items.size() < 2)
    return std::nullopt;

  if (!TryParse(items[0], &code.address, 16))
    return std::nullopt;
  if (!TryParse(items[1], &code.data, 16))
    return std::nullopt;

  return code;
}

}  // namespace Gecko
