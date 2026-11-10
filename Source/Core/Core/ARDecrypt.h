// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <cstddef>
#include <string>
#include <vector>

#include "Core/ActionReplay.h"

namespace ActionReplay
{

struct GameIDAndRegion
{
  // FYI: The "game id" here doesn't seem to match what we are familiar with.
  u32 game_id;
  u32 region;
};

std::optional<GameIDAndRegion> DecryptARCode(const std::string* encrypted_lines, size_t count,
                                             std::vector<AREntry>* result);

}  // namespace ActionReplay
