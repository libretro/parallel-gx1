// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/Types.h"

struct GraphicsModAssetConfig
{
  VideoCommon::CustomAssetLibrary::AssetID m_asset_id;
  VideoCommon::Assets::AssetMap m_map;
};
