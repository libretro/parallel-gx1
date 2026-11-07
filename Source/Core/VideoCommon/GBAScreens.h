// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
class PostProcessing;
}

/* The screens of the GBAs on the controller ports, drawn over the
 * output's bottom right corner when shown.
 *
 * A GBA's thread renders each frame straight into one of three buffers
 * the GPU thread lends it: the GPU's own upload memory where it can stay
 * mapped, plain memory it uploads from otherwise. A finished frame is
 * swapped in through one atomic word, and a buffer goes back to the GBA
 * only once the GPU is done copying from it. */

enum
{
  GBA_SCREENS = 4,
  /* a GBA, a Game Boy, or a Super Game Boy with its border */
  GBA_SCREEN_MAX_WIDTH = 256,
  GBA_SCREEN_MAX_HEIGHT = 224
};

/* The frontend's thread: whether the screens are shown. */
void GBAScreensSetShown(bool shown);

/* A GBA's own threads. Start once its core runs, with what makes its
 * thread let go of a lent buffer: detach(context) has the GBA's thread
 * call GBAScreenDetached, and returns. Stop once the GBA's thread is
 * gone, before the core goes. */
void GBAScreenStart(unsigned gba, void (*detach)(void* context), void* context);
void GBAScreenStop(unsigned gba);

/* The GBA's thread, as a frame ends: hands it over if it was rendered
 * into a lent buffer, and returns the buffer the next one goes into and
 * its stride in pixels, or nullptr to render into its own. */
u32* GBAScreenFrameEnd(unsigned gba, u32 width, u32 height, size_t* stride);
/* The GBA's thread: the lent buffer it renders into now, or nullptr. */
u32* GBAScreenBuffer(unsigned gba, size_t* stride);
/* The GBA's thread, told to let go: renders into its own buffer now. */
void GBAScreenDetached(unsigned gba);

/* The GPU thread, before the output's picture is drawn: takes the frames
 * the GBAs finished; true if any screen is drawn over the picture. */
bool GBAScreensUpdate();
/* The GPU thread, with the output bound: draws them. */
void GBAScreensDraw(VideoCommon::PostProcessing* post_processor, int width, int height);
/* Before the video backend goes: takes the buffers back. */
void GBAScreensRevoke();
