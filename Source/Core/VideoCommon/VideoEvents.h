// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
}

struct PresentInfo
{
  enum class PresentReason
  {
    Immediate,                // FIFO is Presenting the XFB immediately, straight after the XFB copy
    VideoInterface,           // VideoInterface has triggered a present with a new frame
    VideoInterfaceDuplicate,  // VideoInterface has triggered a present with a duplicate frame
  };

  // The number of (unique) frames since the emulated console booted
  u64 frame_count = 0;

  // The number of presents since the video backend was initialized.
  // never goes backwards.
  u64 present_count = 0;

  // The exact emulated time of the when real hardware would have presented this frame
  u64 emulated_timestamp = 0;

  // Where the presentation of the frame was triggered from
  PresentReason reason = PresentReason::Immediate;

  u32 frame_buffer_width;
  u32 frame_buffer_height;
};

struct VideoEvents
{
  // Called when certain video config setting are changed
  Common::HookableEvent<u32> config_changed_event;

  // An event called just before the first draw call of a frame
  Common::HookableEvent<> before_frame_event;

  // An event called after the frame XFB copy begins processing on the host GPU.
  // Useful for "once per frame" usecases.
  // Note: In a few rare cases, games do multiple XFB copies per frame and join them while
  // presenting.
  //       If this matters to your usecase, you should use BeforePresent instead.
  Common::HookableEvent<Core::System&> after_frame_event;

  // An event that is triggered after a frame is presented.
  // The exact timing of this event depends on backend/driver support.
  Common::HookableEvent<PresentInfo&> after_present_event;

  // An end of frame event that runs on the CPU thread
  Common::HookableEvent<> vi_end_field_event;
};

VideoEvents& GetVideoEvents();
