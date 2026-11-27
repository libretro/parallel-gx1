// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Config/ConfigInfo.h"

#include <cstring>

#include "Common/CommonFuncs.h"

namespace Config
{
bool Location::operator==(const Location& other) const
{
  return system == other.system && strcasecmp(section, other.section) == 0 &&
         strcasecmp(key, other.key) == 0;
}

bool Location::operator<(const Location& other) const
{
  if (system != other.system)
    return system < other.system;

  const int section_compare = strcasecmp(section, other.section);
  if (section_compare != 0)
    return section_compare < 0;

  const int key_compare = strcasecmp(key, other.key);
  return key_compare < 0;
}
}  // namespace Config
