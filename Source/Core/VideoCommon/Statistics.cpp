// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Statistics.h"

#include <cstring>
#include <utility>


#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFMemory.h"

Statistics g_stats;

static Common::EventHook s_before_frame_event;

void Statistics::ResetFrame()
{
  this_frame = {};
}

void Statistics::SwapDL()
{
  std::swap(this_frame.num_dl_prims, this_frame.num_prims);
  std::swap(this_frame.num_xf_loads_in_dl, this_frame.num_xf_loads);
  std::swap(this_frame.num_cp_loads_in_dl, this_frame.num_cp_loads);
  std::swap(this_frame.num_bp_loads_in_dl, this_frame.num_bp_loads);
}

void Statistics::Init()
{
  s_before_frame_event = GetVideoEvents().before_frame_event.Register([] { g_stats.ResetFrame(); });
}

void Statistics::Shutdown()
{
  s_before_frame_event.reset();
}
