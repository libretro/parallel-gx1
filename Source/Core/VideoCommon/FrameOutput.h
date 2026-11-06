// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
  virtual ~FrameOutput() {}

  /* The framebuffer this frame's picture goes into, once whoever shows
   * the pictures is done with what it last read from it. */
  virtual AbstractFramebuffer* Acquire() = 0;

  /* The picture is complete: hands it on. */
  virtual void Present() = 0;

  /* Hands on the top-left width x height of a texture as the picture,
   * where it is, in place of one drawn into what Acquire returns; false
   * when this output only hands on its own targets. The caller keeps the
   * texture and its contents as they are for as long as it is the last
   * picture handed on. */
  virtual bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
  {
    return false;
  }

  virtual u32 GetWidth() const = 0;
  virtual u32 GetHeight() const = 0;
  virtual AbstractTextureFormat GetFormat() const = 0;
};
