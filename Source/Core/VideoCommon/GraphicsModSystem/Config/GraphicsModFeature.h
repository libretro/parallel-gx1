// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Matrix.h"

struct GraphicsModFeatureConfig
{
  std::string m_group;
  std::string m_action;
  /* X, Y and Z of action_data, as move and scale take them */
  Common::Vec3 m_action_xyz;
};
