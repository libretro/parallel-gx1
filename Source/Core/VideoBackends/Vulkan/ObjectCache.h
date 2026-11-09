// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/LinearDiskCache.h"

#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/VKStreamBuffer.h"

#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexShaderGen.h"

namespace Vulkan
{
class CommandBufferManager;
class VertexFormat;
class VKTexture;

class ObjectCache
{
public:
  ObjectCache();
  ~ObjectCache();

  // Perform at startup, create descriptor layouts, compiles all static shaders.
  bool Initialize();
  void Shutdown();

  // Descriptor set layout accessor. Used for allocating descriptor sets.
  VkDescriptorSetLayout GetDescriptorSetLayout(DESCRIPTOR_SET_LAYOUT layout) const
  {
    return m_descriptor_set_layouts[layout];
  }

  // Pipeline layout accessor. Used to fill in required field in PipelineInfo.
  VkPipelineLayout GetPipelineLayout(PIPELINE_LAYOUT layout) const
  {
    return m_pipeline_layouts[layout];
  }

  // Staging buffer for textures.
  StreamBuffer* GetTextureUploadBuffer() { return &m_texture_upload_buffer; }

  // Static samplers
  VkSampler GetPointSampler() const { return m_point_sampler; }
  VkSampler GetLinearSampler() const { return m_linear_sampler; }
  VkSampler GetSampler(const SamplerState& info);

  // Render pass cache.
  VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format, u32 multisamples,
                             VkAttachmentLoadOp load_op);

  // Pipeline cache. Used when creating pipelines for drivers to store compiled programs.
  VkPipelineCache GetPipelineCache() const { return m_pipeline_cache; }

  // Clear sampler cache, use when anisotropy mode changes
  // WARNING: Ensure none of the objects from here are in use when calling
  void ClearSamplerCache();

  // Saves the pipeline cache to disk. Call when shutting down.
  void SavePipelineCache();

  // Reload pipeline cache. Call when host config changes.
  void ReloadPipelineCache();

private:
  bool CreateDescriptorSetLayouts();
  void DestroyDescriptorSetLayouts();
  bool CreatePipelineLayouts();
  void DestroyPipelineLayouts();
  bool CreateStaticSamplers();
  void DestroySamplers();
  void DestroyRenderPassCache();
  bool CreatePipelineCache();
  bool LoadPipelineCache();
  bool ValidatePipelineCache(const u8* data, size_t data_length);
  void DestroyPipelineCache();

  std::array<VkDescriptorSetLayout, NUM_DESCRIPTOR_SET_LAYOUTS> m_descriptor_set_layouts = {};
  std::array<VkPipelineLayout, NUM_PIPELINE_LAYOUTS> m_pipeline_layouts = {};

  StreamBuffer m_texture_upload_buffer;

  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;

  /* Samplers by SamplerState::Hex, open-addressed; grown (doubled) when
   * half full, so a lookup never allocates. A failed sampler is kept too,
   * as VK_NULL_HANDLE, so it is not tried again. */
  struct SamplerSlot
  {
    u64 key;
    VkSampler sampler;
    u32 used;
  };
  SamplerSlot* m_samplers = nullptr;
  u32 m_sampler_capacity = 0; /* a power of two */
  u32 m_num_samplers = 0;
  bool GrowSamplers();

  /* Render passes: a handful (formats and load ops), searched in turn. */
  struct RenderPassSlot
  {
    VkFormat color_format;
    VkFormat depth_format;
    u32 multisamples;
    VkAttachmentLoadOp load_op;
    VkRenderPass pass;
  };
  static constexpr u32 MAX_RENDER_PASSES = 64;
  RenderPassSlot m_render_passes[MAX_RENDER_PASSES];
  u32 m_num_render_passes = 0;

  // pipeline cache
  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
  std::string m_pipeline_cache_filename;
};

extern ObjectCache* g_object_cache;

}  // namespace Vulkan
