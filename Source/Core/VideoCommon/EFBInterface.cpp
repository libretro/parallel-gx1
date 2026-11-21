// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/EFBInterface.h"

#include <algorithm>

#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/System.h"

#include "VideoBackends/Software/SWEfbInterface.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoBackendKind.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

static bool ShouldSkipAccess(u16 x, u16 y)
{
  return !g_ActiveConfig.bEFBAccessEnable || x >= EFB_WIDTH || y >= EFB_HEIGHT;
}

void EFBReinterpretPixelData(EFBReinterpretType convtype)
{
  if (g_video_backend_kind == VIDEO_BACKEND_NULL || g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
    return;
  g_framebuffer_manager->ReinterpretPixelData(convtype);
}

struct EFBPeek
{
  u32 x;
  u32 y;
  u32 color;
  float depth;
};

struct EFBPokeColorEvent
{
  u32 x;
  u32 y;
  u32 color;
};

struct EFBPokeDepthEvent
{
  u32 x;
  u32 y;
  float depth;
};

static void PeekColorOnGPU(void* data)
{
  EFBPeek* const peek = static_cast<EFBPeek*>(data);
  peek->color = g_framebuffer_manager->PeekEFBColor(peek->x, peek->y);
}

static void PeekDepthOnGPU(void* data)
{
  EFBPeek* const peek = static_cast<EFBPeek*>(data);
  peek->depth = g_framebuffer_manager->PeekEFBDepth(peek->x, peek->y);
}

static void PokeColorOnGPU(void* data)
{
  const EFBPokeColorEvent* const poke = static_cast<const EFBPokeColorEvent*>(data);
  g_framebuffer_manager->PokeEFBColor(poke->x, poke->y, poke->color);
}

static void PokeDepthOnGPU(void* data)
{
  const EFBPokeDepthEvent* const poke = static_cast<const EFBPokeDepthEvent*>(data);
  g_framebuffer_manager->PokeEFBDepth(poke->x, poke->y, poke->depth);
}

static u32 PeekColorInternal(u16 x, u16 y)
{
  if (g_video_backend_kind == VIDEO_BACKEND_NULL)
    return 0;
  if (g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
    return EfbInterface::PeekColor(x, y);

  EFBPeek peek;
  peek.x = x;
  peek.y = y;
  AsyncRequests::GetInstance()->PushBlockingEvent(PeekColorOnGPU, &peek);
  u32 color = peek.color;

  // a little-endian value is expected to be returned
  color = ((color & 0xFF00FF00) | ((color >> 16) & 0xFF) | ((color << 16) & 0xFF0000));

  if (bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24)
  {
    color = RGBA8ToRGBA6ToRGBA8(color);
  }
  else if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16)
  {
    color = RGBA8ToRGB565ToRGBA8(color);
  }
  if (bpmem.zcontrol.pixel_format != PixelFormat::RGBA6_Z24)
  {
    color |= 0xFF000000;
  }

  return color;
}

u32 EFBPeekColor(u16 x, u16 y)
{
  if (ShouldSkipAccess(x, y))
    return 0;

  u32 color = PeekColorInternal(x, y);

  // check what to do with the alpha channel (GX_PokeAlphaRead)
  PixelEngine::AlphaReadMode alpha_read_mode =
      Core::System::GetInstance().GetPixelEngine().GetAlphaReadMode();

  if (alpha_read_mode == PixelEngine::AlphaReadMode::ReadNone)
  {
    return color;
  }
  else if (alpha_read_mode == PixelEngine::AlphaReadMode::ReadFF)
  {
    return color | 0xFF000000;
  }
  else
  {
    if (alpha_read_mode != PixelEngine::AlphaReadMode::Read00)
    {
      PanicAlert("Invalid PE alpha read mode: %u", (unsigned)(static_cast<u16>(alpha_read_mode)));
    }
    return color & 0x00FFFFFF;
  }
}

static u32 PeekDepthInternal(u16 x, u16 y)
{
  if (g_video_backend_kind == VIDEO_BACKEND_NULL)
    return 0;
  if (g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
    return EfbInterface::PeekDepth(x, y);

  EFBPeek peek;
  peek.x = x;
  peek.y = y;
  AsyncRequests::GetInstance()->PushBlockingEvent(PeekDepthOnGPU, &peek);
  float depth = peek.depth;

  // Depth buffer is inverted for improved precision near far plane
  if (!g_backend_info.bSupportsReversedDepthRange)
    depth = 1.0f - depth;

  // Convert to 24bit depth
  u32 z24depth = std::clamp<u32>(static_cast<u32>(depth * 16777216.0f), 0, 0xFFFFFF);

  if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16)
  {
    // When in RGB565_Z16 mode, EFB Z peeks return a 16bit value, which is presumably a
    // resolved sample from the MSAA buffer.
    // Dolphin doesn't currently emulate the 3 sample MSAA mode (and potentially never will)
    // it just transparently upgrades the framebuffer to 24bit depth and color and whatever
    // level of MSAA and higher Internal Resolution the user has configured.

    // This is mostly transparent, unless the game does an EFB read.
    // But we can simply convert the 24bit depth on the fly to the 16bit depth the game expects.

    return CompressZ16(z24depth, bpmem.zcontrol.zformat);
  }

  return z24depth;
}

u32 EFBPeekDepth(u16 x, u16 y)
{
  if (ShouldSkipAccess(x, y))
    return 0;

  return PeekDepthInternal(x, y);
}

void EFBPokeColor(u16 x, u16 y, u32 poke_data)
{
  if (g_video_backend_kind == VIDEO_BACKEND_NULL || g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
    return;
  if (ShouldSkipAccess(x, y))
    return;

  // Convert to expected format (BGRA->RGBA)
  // TODO: Check alpha, depending on mode?
  EFBPokeColorEvent poke;
  poke.x = x;
  poke.y = y;
  poke.color =
      ((poke_data & 0xFF00FF00) | ((poke_data >> 16) & 0xFF) | ((poke_data << 16) & 0xFF0000));

  AsyncRequests::GetInstance()->PushEvent(PokeColorOnGPU, &poke, sizeof(poke));
}

void EFBPokeDepth(u16 x, u16 y, u32 poke_data)
{
  if (g_video_backend_kind == VIDEO_BACKEND_NULL || g_video_backend_kind == VIDEO_BACKEND_SOFTWARE)
    return;
  if (ShouldSkipAccess(x, y))
    return;

  // Convert to floating-point depth.
  float depth = float(poke_data & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    depth = 1.0f - depth;

  EFBPokeDepthEvent poke;
  poke.x = x;
  poke.y = y;
  poke.depth = depth;
  AsyncRequests::GetInstance()->PushEvent(PokeDepthOnGPU, &poke, sizeof(poke));
}
