// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Matrix.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

namespace GraphicsModActionFactory
{
// Freed with delete; null for an unknown name
GraphicsModAction* Create(const char* name, const Common::Vec3& xyz);
}
