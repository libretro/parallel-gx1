// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBinary.h"

class ShaderCode;

namespace Vulkan
{
class VKShader final : public AbstractShader
{
public:
  /* Both take the SPIR-V's block. */
  VKShader(ShaderStage stage, ShaderBinary spv, VkShaderModule mod, std::string_view name);
  VKShader(ShaderBinary spv, VkPipeline compute_pipeline, std::string_view name);
  ~VKShader();

  VkShaderModule GetShaderModule() const { return m_module; }
  VkPipeline GetComputePipeline() const { return m_compute_pipeline; }
  /* unique among the shaders made since the backend came up: a shader
   * made where a destroyed one was is another shader */
  u32 GetUid() const { return m_uid; }
  const u8* GetBinary(size_t* size) const
  {
    *size = m_spv.size;
    return m_spv.code;
  }

  static VKShader* CreateFromSource(ShaderStage stage, ShaderCode* source,
                                                    std::string_view name);
  static VKShader* CreateFromBinary(ShaderStage stage, const void* data,
                                                    size_t length, std::string_view name);

private:
  ShaderBinary m_spv;
  VkShaderModule m_module;
  VkPipeline m_compute_pipeline;
  u32 m_uid;
};

}  // namespace Vulkan
