// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/GeckoCode.h"

namespace Common
{
class IniFile;
}

namespace Gecko
{
// The codes of both files into list, whose earlier codes are freed.
void LoadCodes(const Common::IniFile& globalIni, const Common::IniFile& localIni,
               GeckoCodeList* list);
}  // namespace Gecko
