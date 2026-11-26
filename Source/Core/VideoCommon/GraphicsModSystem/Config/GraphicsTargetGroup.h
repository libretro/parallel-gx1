// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

// Zeroed is empty; freed with its mod (FreeGraphicsModConfig)
struct GraphicsTargetGroupConfig
{
  char* m_name;  // owned, null when the mod names none
  // Grown by doubling
  GraphicsTargetConfig* m_targets;
  u32 m_target_count;
  u32 m_target_capacity;
};
