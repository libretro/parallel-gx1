// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PerfQueryBase.h"

#include <memory>

#include "VideoCommon/VideoConfig.h"

PerfQueryBase* g_perf_query = nullptr;

bool PerfQueryBase::ShouldEmulate()
{
  return g_ActiveConfig.bPerfQueriesEnable;
}
