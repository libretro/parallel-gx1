// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>

namespace VideoCommon::Assets
{
/* key -> UTF-8 file path */
using AssetMap = std::map<std::string, std::string>;
}
