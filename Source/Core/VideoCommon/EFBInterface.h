// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

enum class EFBReinterpretType;

/* CPU access to the EFB, for the backend in use (g_video_backend_kind):
 * the hardware backends go through the framebuffer manager, the software
 * renderer reads its own EFB and the null renderer reads zero. */
void EFBReinterpretPixelData(EFBReinterpretType convtype);
u32 EFBPeekColor(u16 x, u16 y);
u32 EFBPeekDepth(u16 x, u16 y);
void EFBPokeColor(u16 x, u16 y, u32 color);
void EFBPokeDepth(u16 x, u16 y, u32 depth);
