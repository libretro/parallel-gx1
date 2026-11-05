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

static bool clear_scissors;

void Statistics::ResetFrame()
{
  this_frame = {};
  clear_scissors = true;
  if (scissors.size() > 1)
  {
    scissors.erase(scissors.begin(), scissors.end() - 1);
  }
}

void Statistics::SwapDL()
{
  std::swap(this_frame.num_dl_prims, this_frame.num_prims);
  std::swap(this_frame.num_xf_loads_in_dl, this_frame.num_xf_loads);
  std::swap(this_frame.num_cp_loads_in_dl, this_frame.num_cp_loads);
  std::swap(this_frame.num_bp_loads_in_dl, this_frame.num_bp_loads);
}

void Statistics::AddScissorRect()
{
  if (clear_scissors)
  {
    scissors.clear();
    clear_scissors = false;
  }

  BPFunctions::ScissorResult scissor = BPFunctions::ComputeScissorRects(
      bpmem.scissorTL, bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  bool add;
  if (scissors.empty())
  {
    add = true;
  }
  else
  {
    if (allow_duplicate_scissors)
    {
      // Only check the last entry
      add = !scissors.back().Matches(scissor, show_scissors, show_viewports);
    }
    else
    {
      add = std::ranges::find_if(scissors, [&](auto& s) {
              return s.Matches(scissor, show_scissors, show_viewports);
            }) == scissors.end();
    }
  }
  if (add)
    scissors.push_back(std::move(scissor));
}

void Statistics::Init()
{
  s_before_frame_event = GetVideoEvents().before_frame_event.Register([] { g_stats.ResetFrame(); });
}

void Statistics::Shutdown()
{
  s_before_frame_event.reset();
}
