// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBinary.h"
#include "VideoCommon/ShaderGenCommon.h"

namespace Vulkan::ShaderCompiler
{
/* Compiles a stage to SPIR-V into *spirv, which owns rslc's result block
 * then: the header is written in front of the source's text. False, with
 * the failure logged and dumped, if rslc refused it. */
bool Compile(ShaderStage stage, ShaderCode* source, ShaderBinary* spirv);
}  // namespace Vulkan::ShaderCompiler
