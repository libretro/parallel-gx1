// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

struct GraphicsTargetGroupConfig
{
  std::string m_name;
  std::vector<GraphicsTargetConfig> m_targets;
};
