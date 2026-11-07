// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBinary.h"

namespace Vulkan::ShaderCompiler
{
/* Compiles a stage to SPIR-V into *spirv, which owns rslc's result block
 * then. False, with the failure logged and dumped, if rslc refused it. */
bool Compile(ShaderStage stage, std::string_view source_code, ShaderBinary* spirv);
}  // namespace Vulkan::ShaderCompiler
