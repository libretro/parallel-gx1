// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Matrix.h"

// Zeroed is empty; freed with its mod (FreeGraphicsModConfig)
struct GraphicsModFeatureConfig
{
  char* m_group;   // owned, null when the mod names none
  char* m_action;  // owned, null when the mod names none
  /* X, Y and Z of action_data, as move and scale take them */
  Common::Vec3 m_action_xyz;
};
