// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>

#include "Core/ActionReplay.h"

namespace ActionReplay
{

struct GameIDAndRegion
{
  // FYI: The "game id" here doesn't seem to match what we are familiar with.
  u32 game_id;
  u32 region;
};

// The encrypted lines, ENCRYPTED_LINE_SIZE characters each and one after the other, decrypted
// behind the code's lines.
constexpr size_t ENCRYPTED_LINE_SIZE = 13;
std::optional<GameIDAndRegion> DecryptARCode(const char* encrypted_lines, size_t count,
                                             ARCode* code);

}  // namespace ActionReplay
