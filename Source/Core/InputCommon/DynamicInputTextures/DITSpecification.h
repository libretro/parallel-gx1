// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "InputCommon/DynamicInputTextures/DITData.h"

namespace InputCommon::DynamicInputTextures
{
/* Reads json_file, a specification 1 dynamic input texture file, into input_textures; the
 * images it names are under base_path. False, with the reason logged, where it is not one. */
bool LoadSpecification(const std::string& json_file, std::vector<Data>& input_textures,
                       const std::string& base_path);
}
