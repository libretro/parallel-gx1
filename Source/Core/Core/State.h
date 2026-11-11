// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulator state saving support.

#pragma once

#include "Common/ChunkFile.h"

namespace Core
{
class System;
}

namespace State
{
void DoState(Core::System& system, PointerWrap& p);
}  // namespace State
