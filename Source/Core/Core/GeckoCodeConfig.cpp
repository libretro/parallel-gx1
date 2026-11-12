// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/GeckoCodeConfig.h"

#include <algorithm>
#include <optional>
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
      switch ((line)[0])
      {
      // enabled or disabled code: "+$name [creator]" or "$name [creator]"
      case '+':
      case '$':
      {
        if (!gcode.name.empty())
          gcodes.push_back(gcode);
        gcode = GeckoCode();
        gcode.enabled = line[0] == '+';
        gcode.user_defined = (ini == &localIni);
        const size_t start = std::min<size_t>(gcode.enabled ? 2 : 1, line.size());
        // the code name stops at '[', where the creator's name begins
        const size_t open = line.find('[', start);
        gcode.name = StripWhitespace(
            line.substr(start, open == std::string::npos ? std::string::npos : open - start));
        if (open != std::string::npos)
        {
          const size_t close = line.find(']', open + 1);
          gcode.creator = line.substr(open + 1, close == std::string::npos ? std::string::npos :
                                                                             close - open - 1);
        }
        break;
      }

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
