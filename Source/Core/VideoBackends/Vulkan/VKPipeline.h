// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractPipeline.h"

namespace Vulkan
{
/* A pipeline's cull mode and depth state, set per draw when the device
 * can (VKPipeline::GetDynamicState): the VkCullModeFlags, depth test,
 * depth write and VkCompareOp, and a bit saying they are set this way. */
#define VK_DYNAMIC_CULL_MASK 0x3u
#define VK_DYNAMIC_DEPTH_TEST (1u << 2)
#define VK_DYNAMIC_DEPTH_WRITE (1u << 3)
#define VK_DYNAMIC_DEPTH_OP_SHIFT 4
#define VK_DYNAMIC_DEPTH_OP_MASK (0x7u << VK_DYNAMIC_DEPTH_OP_SHIFT)
#define VK_DYNAMIC_USED (1u << 31)

struct VKSharedPipeline;

class VKPipeline final : public AbstractPipeline
{
public:
  explicit VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                      VkPipelineLayout pipeline_layout, AbstractPipelineUsage usage,
                      u32 dynamic_state, VKSharedPipeline* shared);
  ~VKPipeline() override;

  VkPipeline GetVkPipeline() const { return m_pipeline; }
  VkPipelineLayout GetVkPipelineLayout() const { return m_pipeline_layout; }
  AbstractPipelineUsage GetUsage() const { return m_usage; }
  /* 0, or the VK_DYNAMIC_* state to set before drawing with it */
  u32 GetDynamicState() const { return m_dynamic_state; }
  static std::unique_ptr<VKPipeline> Create(const AbstractPipelineConfig& config);
  /* Forgets every shared pipeline; only with none alive, as the backend
   * comes up. */
  static void ResetSharedPipelines();

private:
  VkPipeline m_pipeline;
  VkPipelineLayout m_pipeline_layout;
  VKSharedPipeline* m_shared;
  AbstractPipelineUsage m_usage;
  u32 m_dynamic_state;
};

}  // namespace Vulkan
