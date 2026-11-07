// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoCommon/PerfQueryBase.h"

namespace SW
{
/* The rasterizer counts as it draws: nothing to start, stop or wait for. */
class PerfQuery final : public PerfQueryBase
{
public:
  void ResetQuery() { EfbInterface::ResetPerfQuery(); }
  u32 GetQueryResult(PerfQueryType type) { return EfbInterface::GetPerfQueryResult(type); }
};

}  // namespace SW
