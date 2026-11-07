// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractDeleters.h"

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureConfig.h"

class AbstractFramebuffer;
class AbstractTexture;

/* Where a backend draws the picture it presents, and what hands that
 * picture to whoever shows it: the frontend owns the window, so this is
 * a render target the frontend reads in place, not a swap chain. */
class FrameOutput
{
public:
  /* Each backend has one output class, and these call it directly
   * (g_video_backend_kind, DolphinLibretro/Output.cpp); what one does
   * not do itself, these answer. */

  /* The framebuffer this frame's picture goes into, once whoever shows
   * the pictures is done with what it last read from it. */
  AbstractFramebuffer* Acquire();

  /* The picture is complete: hands it on. */
  void Present();

  /* Hands on the top-left width x height of a texture as the picture,
   * where it is, in place of one drawn into what Acquire returns; false
   * when this output only hands on its own targets. The caller keeps the
   * texture and its contents as they are for as long as it is the last
   * picture handed on. */
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height);

  /* Whoever shows the pictures is about to read the last one handed on,
   * again or for the first time, the core having gone on drawing since:
   * what it reads is left the way it reads it. */
  void KeepReadable();

  /* Whether a texture handed on with PresentTexture may still be read by
   * whoever shows the pictures, now or later: the caller keeps it alive
   * until it is not. */
  bool IsTextureHeld(const AbstractTexture* texture);

  /* An output that publishes pictures from another thread: hands the
   * newest one over to whoever shows them, on that one's thread; true if
   * there was one. */
  bool HandOver();

  u32 GetWidth() const;
  u32 GetHeight() const;
  AbstractTextureFormat GetFormat() const { return AbstractTextureFormat::RGBA8; }
};
