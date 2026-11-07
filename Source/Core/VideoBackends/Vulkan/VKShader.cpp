// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKShader.h"

#include <retro_atomic.h>

#include "Common/Align.h"

#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
static retro_atomic_int_t s_next_uid;

static u32 NextUid()
{
  return (u32)retro_atomic_fetch_add_int(&s_next_uid, 1) + 1u;
}

VKShader::VKShader(ShaderStage stage, ShaderBinary spv, VkShaderModule mod, std::string_view name)
    : AbstractShader(stage), m_spv(spv), m_module(mod),
      m_compute_pipeline(VK_NULL_HANDLE), m_name(name), m_uid(NextUid())
{
  if (!m_name.empty() && g_backend_info.bSupportsSettingObjectNames)
  {
    VkDebugUtilsObjectNameInfoEXT name_info = {};
    name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
    name_info.objectHandle = reinterpret_cast<uint64_t>(m_module);
    name_info.pObjectName = m_name.data();
    vkSetDebugUtilsObjectNameEXT(g_vulkan_context->GetDevice(), &name_info);
  }
}

VKShader::VKShader(ShaderBinary spv, VkPipeline compute_pipeline, std::string_view name)
    : AbstractShader(ShaderStage::Compute), m_spv(spv), m_module(VK_NULL_HANDLE),
      m_compute_pipeline(compute_pipeline), m_name(name), m_uid(NextUid())
{
  if (!m_name.empty() && g_backend_info.bSupportsSettingObjectNames)
  {
    VkDebugUtilsObjectNameInfoEXT name_info = {};
    name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType = VK_OBJECT_TYPE_PIPELINE;
    name_info.objectHandle = reinterpret_cast<uint64_t>(m_compute_pipeline);
    name_info.pObjectName = m_name.data();
    vkSetDebugUtilsObjectNameEXT(g_vulkan_context->GetDevice(), &name_info);
  }
}

VKShader::~VKShader()
{
  if (m_stage != ShaderStage::Compute)
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), m_module, nullptr);
  else
    vkDestroyPipeline(g_vulkan_context->GetDevice(), m_compute_pipeline, nullptr);
  ShaderBinaryFree(&m_spv);
}

/* Takes the SPIR-V's block: the shader keeps it, or it is freed. */
static std::unique_ptr<VKShader> CreateShaderObject(ShaderStage stage, ShaderBinary spv,
                                                    std::string_view name)
{
  VkShaderModuleCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spv.size;
  info.pCode = reinterpret_cast<const u32*>(spv.code);

  VkShaderModule mod;
  VkResult res = vkCreateShaderModule(g_vulkan_context->GetDevice(), &info, nullptr, &mod);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule failed: ");
    ShaderBinaryFree(&spv);
    return nullptr;
  }

  // If it's a graphics shader, we defer pipeline creation.
  if (stage != ShaderStage::Compute)
    return std::make_unique<VKShader>(stage, spv, mod, name);

  // If it's a compute shader, we create the pipeline straight away.
  const VkComputePipelineCreateInfo pipeline_info = {
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      nullptr,
      0,
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
       mod, "main", nullptr},
      g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_COMPUTE),
      VK_NULL_HANDLE,
      -1};

  VkPipeline pipeline;
  res = vkCreateComputePipelines(g_vulkan_context->GetDevice(), g_object_cache->GetPipelineCache(),
                                 1, &pipeline_info, nullptr, &pipeline);

  // Shader module is no longer needed, now it is compiled to a pipeline.
  vkDestroyShaderModule(g_vulkan_context->GetDevice(), mod, nullptr);

  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateComputePipelines failed: ");
    ShaderBinaryFree(&spv);
    return nullptr;
  }

  return std::make_unique<VKShader>(spv, pipeline, name);
}

std::unique_ptr<VKShader> VKShader::CreateFromSource(ShaderStage stage, ShaderCode* source,
                                                     std::string_view name)
{
  ShaderBinary spv;
  if (!ShaderCompiler::Compile(stage, source, &spv))
    return nullptr;
  return CreateShaderObject(stage, spv, name);
}

std::unique_ptr<VKShader> VKShader::CreateFromBinary(ShaderStage stage, const void* data,
                                                     size_t length, std::string_view name)
{
  ShaderBinary spv;
  /* SPIR-V is whole words */
  if ((length % sizeof(u32)) != 0 || !ShaderBinaryCopy(&spv, data, length))
    return nullptr;
  return CreateShaderObject(stage, spv, name);
}

}  // namespace Vulkan
