// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"

namespace Common
{
/* A PNG decoded to R, G, B, A bytes */
bool LoadPNG(const u8* input, size_t input_size, Common::UniqueBuffer<u8>* data_out,
             u32* width_out, u32* height_out);

/* R, G, B, A rows stride bytes apart, written to path as a PNG */
bool SavePNG(const std::string& path, const u8* input, u32 width, u32 height, u32 stride);
}  // namespace Common
