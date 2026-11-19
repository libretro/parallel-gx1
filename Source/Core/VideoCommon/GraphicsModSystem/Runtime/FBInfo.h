// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureDecoder.h"

struct FBInfo
{
  u32 m_height = 0;
  u32 m_width = 0;
  TextureFormat m_texture_format = TextureFormat::I4;
};
