// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/ObjectCache.h"

#include "VideoBackends/Vulkan/VKPipeline.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <type_traits>

#include "Common/FileUtil.h"
#include "Common/LinearDiskCache.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/VKStreamBuffer.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/VideoCommon.h"

namespace Vulkan
{
ObjectCache* g_object_cache = nullptr;

ObjectCache::ObjectCache() = default;

ObjectCache::~ObjectCache()
{
  VKPipeline::DestroySharedLibraries();
  DestroyPipelineCache();
  DestroySamplers();
  DestroyPipelineLayouts();
  DestroyDescriptorSetLayouts();
  DestroyRenderPassCache();
}

bool ObjectCache::Initialize()
{
  VKPipeline::ResetSharedPipelines();

  if (!CreateDescriptorSetLayouts())
    return false;

  if (!CreatePipelineLayouts())
    return false;

  if (!CreateStaticSamplers())
    return false;

  if (!m_texture_upload_buffer.Init(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_UPLOAD_BUFFER_SIZE,
                                    VK_HEAP_UPLOAD_CACHED))
  {
    PanicAlertFmt("Failed to create texture upload buffer");
    return false;
  }

  if (g_ActiveConfig.bShaderCache)
  {
    if (!LoadPipelineCache())
      return false;
  }
  else
  {
    if (!CreatePipelineCache())
      return false;
  }

  return true;
}

void ObjectCache::Shutdown()
{
  if (g_ActiveConfig.bShaderCache && m_pipeline_cache != VK_NULL_HANDLE)
    SavePipelineCache();
}

void ObjectCache::ClearSamplerCache()
{
  u32 i;
  for (i = 0; i < m_sampler_capacity; i++)
  {
    if (m_samplers[i].used && m_samplers[i].sampler != VK_NULL_HANDLE)
      vkDestroySampler(g_vulkan_context->GetDevice(), m_samplers[i].sampler, nullptr);
    m_samplers[i].used = 0;
  }
  m_num_samplers = 0;
}

static u32 SamplerHash(u64 key)
{
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return static_cast<u32>(key);
}

/* Doubles the table (or makes the first), placing what it held again. */
bool ObjectCache::GrowSamplers()
{
  const u32 capacity = m_sampler_capacity ? m_sampler_capacity * 2 : 256;
  SamplerSlot* const samplers =
      static_cast<SamplerSlot*>(calloc(capacity, sizeof(SamplerSlot)));
  u32 i;
  if (!samplers)
    return false;
  for (i = 0; i < m_sampler_capacity; i++)
  {
    u32 j;
    if (!m_samplers[i].used)
      continue;
    j = SamplerHash(m_samplers[i].key) & (capacity - 1);
    while (samplers[j].used)
      j = (j + 1) & (capacity - 1);
    samplers[j] = m_samplers[i];
  }
  free(m_samplers);
  m_samplers = samplers;
  m_sampler_capacity = capacity;
  return true;
}

void ObjectCache::DestroySamplers()
{
  ClearSamplerCache();
  free(m_samplers);
  m_samplers = nullptr;
  m_sampler_capacity = 0;

  if (m_point_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(g_vulkan_context->GetDevice(), m_point_sampler, nullptr);
    m_point_sampler = VK_NULL_HANDLE;
  }

  if (m_linear_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(g_vulkan_context->GetDevice(), m_linear_sampler, nullptr);
    m_linear_sampler = VK_NULL_HANDLE;
  }
}

bool ObjectCache::CreateDescriptorSetLayouts()
{
  // The geometry shader buffer must be last in this binding set, as we don't include it
  // if geometry shaders are not supported by the device. See the decrement below.
  static const std::array<VkDescriptorSetLayoutBinding, 4> standard_ubo_bindings{{
      {UBO_DESCRIPTOR_SET_BINDING_PS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_VS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_CUST, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_GS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_GEOMETRY_BIT},
  }};

  constexpr u32 MAX_PIXEL_SAMPLER_ARRAY_SIZE = 8;
  constexpr u32 TOTAL_PIXEL_SAMPLER_BINDINGS =
      1 + (VideoCommon::MAX_PIXEL_SHADER_SAMPLERS - MAX_PIXEL_SAMPLER_ARRAY_SIZE);
  static_assert(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS == 16, "Update descriptor sampler bindings");

  static const std::array<VkDescriptorSetLayoutBinding, TOTAL_PIXEL_SAMPLER_BINDINGS>
      standard_sampler_bindings{{
          {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_PIXEL_SAMPLER_ARRAY_SIZE,
           VK_SHADER_STAGE_FRAGMENT_BIT},
          {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      }};

  // The dynamic veretex loader's vertex buffer must be last here, for similar reasons
  static const std::array<VkDescriptorSetLayoutBinding, 2> standard_ssbo_bindings{{
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
  }};

  static const std::array<VkDescriptorSetLayoutBinding, 1> utility_ubo_bindings{{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
  }};

  // Utility samplers aren't dynamically indexed.
  static const std::array<VkDescriptorSetLayoutBinding, 9> utility_sampler_bindings{{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {8, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
  }};

  static const std::array<VkDescriptorSetLayoutBinding, 19> compute_set_bindings{{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {9, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {10, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
  }};

  std::array<VkDescriptorSetLayoutBinding, 4> ubo_bindings = standard_ubo_bindings;

  std::array<VkDescriptorSetLayoutCreateInfo, NUM_DESCRIPTOR_SET_LAYOUTS> create_infos{{
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(ubo_bindings.size()), ubo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_sampler_bindings.size()), standard_sampler_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_ssbo_bindings.size()), standard_ssbo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_ubo_bindings.size()), utility_ubo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_sampler_bindings.size()), utility_sampler_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(compute_set_bindings.size()), compute_set_bindings.data()},
  }};

  // Don't set the GS bit if geometry shaders aren't available.
  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    if (g_backend_info.bSupportsGeometryShaders)
      ubo_bindings[UBO_DESCRIPTOR_SET_BINDING_GS].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
    else
      ubo_bindings[UBO_DESCRIPTOR_SET_BINDING_GS].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  else if (!g_backend_info.bSupportsGeometryShaders)
  {
    create_infos[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS].bindingCount--;
  }

  // Remove the dynamic vertex loader's buffer if it'll never be needed
  if (!g_backend_info.bSupportsDynamicVertexLoader)
    create_infos[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS].bindingCount--;

  for (size_t i = 0; i < create_infos.size(); i++)
  {
    VkResult res = vkCreateDescriptorSetLayout(g_vulkan_context->GetDevice(), &create_infos[i],
                                               nullptr, &m_descriptor_set_layouts[i]);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateDescriptorSetLayout failed: ");
      return false;
    }
  }

  return true;
}

void ObjectCache::DestroyDescriptorSetLayouts()
{
  for (VkDescriptorSetLayout layout : m_descriptor_set_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), layout, nullptr);
  }
}

bool ObjectCache::CreatePipelineLayouts()
{
  // Descriptor sets for each pipeline layout.
  // In the standard set, the SSBO must be the last descriptor, as we do not include it
  // when fragment stores and atomics are not supported by the device.
  const std::array<VkDescriptorSetLayout, 3> standard_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS],
  };
  const std::array<VkDescriptorSetLayout, 3> uber_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS],
  };
  const std::array<VkDescriptorSetLayout, 2> utility_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS],
  };
  const std::array<VkDescriptorSetLayout, 1> compute_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_COMPUTE],
  };

  // Info for each pipeline layout
  std::array<VkPipelineLayoutCreateInfo, NUM_PIPELINE_LAYOUTS> pipeline_layout_info{{
      // Standard
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_sets.size()), standard_sets.data(), 0, nullptr},

      // Uber
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(uber_sets.size()), uber_sets.data(), 0, nullptr},

      // Utility
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_sets.size()), utility_sets.data(), 0, nullptr},

      // Compute
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(compute_sets.size()), compute_sets.data(), 0, nullptr},
  }};

  const bool ssbos_in_standard =
      g_backend_info.bSupportsBBox || g_ActiveConfig.UseVSForLinePointExpand();

  // If bounding box is unsupported, don't bother with the SSBO descriptor set.
  if (!ssbos_in_standard)
    pipeline_layout_info[PIPELINE_LAYOUT_STANDARD].setLayoutCount--;
  // If neither SSBO-using feature is supported, skip in ubershaders too
  if (!ssbos_in_standard && !g_backend_info.bSupportsDynamicVertexLoader)
    pipeline_layout_info[PIPELINE_LAYOUT_UBER].setLayoutCount--;

  for (size_t i = 0; i < pipeline_layout_info.size(); i++)
  {
    VkResult res;
    if ((res = vkCreatePipelineLayout(g_vulkan_context->GetDevice(), &pipeline_layout_info[i],
                                      nullptr, &m_pipeline_layouts[i])) != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreatePipelineLayout failed: ");
      return false;
    }
  }

  return true;
}

void ObjectCache::DestroyPipelineLayouts()
{
  for (VkPipelineLayout layout : m_pipeline_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), layout, nullptr);
  }
}

bool ObjectCache::CreateStaticSamplers()
{
  VkSamplerCreateInfo create_info = {
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,    // VkStructureType         sType
      nullptr,                                  // const void*             pNext
      0,                                        // VkSamplerCreateFlags    flags
      VK_FILTER_NEAREST,                        // VkFilter                magFilter
      VK_FILTER_NEAREST,                        // VkFilter                minFilter
      VK_SAMPLER_MIPMAP_MODE_NEAREST,           // VkSamplerMipmapMode     mipmapMode
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeU
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeV
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,    // VkSamplerAddressMode    addressModeW
      0.0f,                                     // float                   mipLodBias
      VK_FALSE,                                 // VkBool32                anisotropyEnable
      1.0f,                                     // float                   maxAnisotropy
      VK_FALSE,                                 // VkBool32                compareEnable
      VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
      std::numeric_limits<float>::min(),        // float                   minLod
      std::numeric_limits<float>::max(),        // float                   maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
      VK_FALSE                                  // VkBool32                unnormalizedCoordinates
  };

  VkResult res =
      vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_point_sampler);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
    return false;
  }

  // Most fields are shared across point<->linear samplers, so only change those necessary.
  create_info.minFilter = VK_FILTER_LINEAR;
  create_info.magFilter = VK_FILTER_LINEAR;
  create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_linear_sampler);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
    return false;
  }

  return true;
}

VkSampler ObjectCache::GetSampler(const SamplerState& info)
{
  const u64 key = info.Hex();
  u32 slot = 0;
  if (m_sampler_capacity)
  {
    slot = SamplerHash(key) & (m_sampler_capacity - 1);
    while (m_samplers[slot].used)
    {
      if (m_samplers[slot].key == key)
        return m_samplers[slot].sampler;
      slot = (slot + 1) & (m_sampler_capacity - 1);
    }
  }

  static constexpr std::array<VkFilter, 4> filters = {{VK_FILTER_NEAREST, VK_FILTER_LINEAR}};
  static constexpr std::array<VkSamplerMipmapMode, 2> mipmap_modes = {
      {VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR}};
  static constexpr std::array<VkSamplerAddressMode, 4> address_modes = {
      {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
       VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT}};

  VkSamplerCreateInfo create_info = {
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,              // VkStructureType         sType
      nullptr,                                            // const void*             pNext
      0,                                                  // VkSamplerCreateFlags    flags
      filters[u32(info.tm0.mag_filter.Value())],          // VkFilter                magFilter
      filters[u32(info.tm0.min_filter.Value())],          // VkFilter                minFilter
      mipmap_modes[u32(info.tm0.mipmap_filter.Value())],  // VkSamplerMipmapMode mipmapMode
      address_modes[u32(info.tm0.wrap_u.Value())],        // VkSamplerAddressMode    addressModeU
      address_modes[u32(info.tm0.wrap_v.Value())],        // VkSamplerAddressMode    addressModeV
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,              // VkSamplerAddressMode    addressModeW
      info.tm0.lod_bias / 256.0f,                         // float                   mipLodBias
      VK_FALSE,                                 // VkBool32                anisotropyEnable
      0.0f,                                     // float                   maxAnisotropy
      VK_FALSE,                                 // VkBool32                compareEnable
      VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
      info.tm1.min_lod / 16.0f,                 // float                   minLod
      info.tm1.max_lod / 16.0f,                 // float                   maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
      VK_FALSE                                  // VkBool32                unnormalizedCoordinates
  };

  // Can we use anisotropic filtering with this sampler?
  if (info.tm0.anisotropic_filtering != 0 && g_vulkan_context->SupportsAnisotropicFiltering())
  {
    // Cap anisotropy to device limits.
    create_info.anisotropyEnable = VK_TRUE;
    create_info.maxAnisotropy = std::min(static_cast<float>(1 << info.tm0.anisotropic_filtering),
                                         g_vulkan_context->GetMaxSamplerAnisotropy());
  }

  VkSampler sampler = VK_NULL_HANDLE;
  VkResult res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &sampler);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");

  // Store it even if it failed
  if ((m_num_samplers + 1) * 2 > m_sampler_capacity)
  {
    if (!GrowSamplers())
      return sampler; /* not kept: made again the next time */
    slot = SamplerHash(key) & (m_sampler_capacity - 1);
    while (m_samplers[slot].used)
      slot = (slot + 1) & (m_sampler_capacity - 1);
  }
  m_samplers[slot].key = key;
  m_samplers[slot].sampler = sampler;
  m_samplers[slot].used = 1;
  m_num_samplers++;
  return sampler;
}

VkRenderPass ObjectCache::GetRenderPass(VkFormat color_format, VkFormat depth_format,
                                        u32 multisamples, VkAttachmentLoadOp load_op)
{
  u32 i;
  for (i = 0; i < m_num_render_passes; i++)
  {
    const RenderPassSlot* const slot = &m_render_passes[i];
    if (slot->color_format == color_format && slot->depth_format == depth_format &&
        slot->multisamples == multisamples && slot->load_op == load_op)
    {
      return slot->pass;
    }
  }
  if (m_num_render_passes == MAX_RENDER_PASSES)
  {
    ERROR_LOG_FMT(VIDEO, "Too many Vulkan render passes");
    return VK_NULL_HANDLE;
  }

  /* the colour attachment, then depth: as the framebuffer lists their views */
  VkAttachmentDescription attachments[2];
  VkAttachmentReference color_reference;
  VkAttachmentReference depth_reference;
  u32 num_attachments = 0;
  const bool has_color = color_format != VK_FORMAT_UNDEFINED;
  const bool has_depth = depth_format != VK_FORMAT_UNDEFINED;
  if (has_color)
  {
    color_reference.attachment = num_attachments;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[num_attachments++] = {
        0, color_format, static_cast<VkSampleCountFlagBits>(multisamples), load_op,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  }
  if (has_depth)
  {
    depth_reference.attachment = num_attachments;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[num_attachments++] = {
        0, depth_format, static_cast<VkSampleCountFlagBits>(multisamples), load_op,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  }

  VkSubpassDescription subpass = {0,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  0,
                                  nullptr,
                                  has_color ? 1u : 0u,
                                  has_color ? &color_reference : nullptr,
                                  nullptr,
                                  has_depth ? &depth_reference : nullptr,
                                  0,
                                  nullptr};
  VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      nullptr,
                                      0,
                                      num_attachments,
                                      attachments,
                                      1,
                                      &subpass,
                                      0,
                                      nullptr};

  VkRenderPass pass;
  VkResult res = vkCreateRenderPass(g_vulkan_context->GetDevice(), &pass_info, nullptr, &pass);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
    return VK_NULL_HANDLE;
  }

  RenderPassSlot* const slot = &m_render_passes[m_num_render_passes++];
  slot->color_format = color_format;
  slot->depth_format = depth_format;
  slot->multisamples = multisamples;
  slot->load_op = load_op;
  slot->pass = pass;
  return pass;
}

void ObjectCache::DestroyRenderPassCache()
{
  u32 i;
  for (i = 0; i < m_num_render_passes; i++)
    vkDestroyRenderPass(g_vulkan_context->GetDevice(), m_render_passes[i].pass, nullptr);
  m_num_render_passes = 0;
}

bool ObjectCache::CreatePipelineCache()
{
  // Vulkan pipeline caches can be shared between games for shader compile time reduction.
  // This assumes that drivers don't create all pipelines in the cache on load time, only
  // when a lookup occurs that matches a pipeline (or pipeline data) in the cache.
  m_pipeline_cache_filename = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  VkPipelineCacheCreateInfo info = {
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
      nullptr,                                       // const void*                pNext
      0,                                             // VkPipelineCacheCreateFlags flags
      0,                                             // size_t                     initialDataSize
      nullptr                                        // const void*                pInitialData
  };

  VkResult res =
      vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed: ");
  return false;
}

bool ObjectCache::LoadPipelineCache()
{
  // The cache is made from the stored blob where it lies in the mapped file.
  struct Reader final : Common::LinearDiskCacheReader<u32, u8>
  {
    explicit Reader(ObjectCache* cache_) : cache(cache_) {}
    void Read(const u32& key, const u8* value, u32 value_size) override
    {
      if (++count != 1 || !value_size)
        return;
      if (!cache->ValidatePipelineCache(value, value_size))
      {
        invalid = true;
        return;
      }
      VkPipelineCacheCreateInfo info = {
          VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
          nullptr,                                       // const void*                pNext
          0,                                             // VkPipelineCacheCreateFlags flags
          value_size,                                    // size_t initialDataSize
          value                                          // const void*                pInitialData
      };
      result = vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr,
                                     &cache->m_pipeline_cache);
    }
    ObjectCache* cache;
    VkResult result = VK_INCOMPLETE;
    u32 count = 0;
    bool invalid = false;
  };

  // We have to keep the pipeline cache file name around since when we save it
  // we delete the old one, by which time the game's unique ID is already cleared.
  m_pipeline_cache_filename = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  Reader reader(this);
  {
    Common::LinearDiskCache<u32, u8> disk_cache;
    disk_cache.OpenAndRead(m_pipeline_cache_filename, reader);
  }

  if (reader.invalid)
  {
    // Don't use this data. In fact, we should delete it to prevent it from being used next time.
    File::Delete(m_pipeline_cache_filename);
    return CreatePipelineCache();
  }
  if (reader.count == 1 && reader.result == VK_SUCCESS)
    return true;
  if (reader.count > 1 && m_pipeline_cache != VK_NULL_HANDLE)
  {
    vkDestroyPipelineCache(g_vulkan_context->GetDevice(), m_pipeline_cache, nullptr);
    m_pipeline_cache = VK_NULL_HANDLE;
  }
  if (reader.result != VK_INCOMPLETE && reader.result != VK_SUCCESS)
    LOG_VULKAN_ERROR(reader.result, "vkCreatePipelineCache failed, trying empty cache: ");
  return CreatePipelineCache();
}

// Based on Vulkan 1.0 specification,
// Table 9.1. Layout for pipeline cache header version VK_PIPELINE_CACHE_HEADER_VERSION_ONE
// NOTE: This data is assumed to be in little-endian format.
#pragma pack(push, 4)
struct VK_PIPELINE_CACHE_HEADER
{
  u32 header_length;
  u32 header_version;
  u32 vendor_id;
  u32 device_id;
  u8 uuid[VK_UUID_SIZE];
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable<VK_PIPELINE_CACHE_HEADER>::value,
              "VK_PIPELINE_CACHE_HEADER must be trivially copyable");

bool ObjectCache::ValidatePipelineCache(const u8* data, size_t data_length)
{
  if (data_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header");
    return false;
  }

  VK_PIPELINE_CACHE_HEADER header;
  std::memcpy(&header, data, sizeof(header));
  if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header length");
    return false;
  }

  if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header version");
    return false;
  }

  if (header.vendor_id != g_vulkan_context->GetDeviceInfo().vendorID)
  {
    ERROR_LOG_FMT(
        VIDEO, "Pipeline cache failed validation: Incorrect vendor ID (file: {:#X}, device: {:#X})",
        header.vendor_id, g_vulkan_context->GetDeviceInfo().vendorID);
    return false;
  }

  if (header.device_id != g_vulkan_context->GetDeviceInfo().deviceID)
  {
    ERROR_LOG_FMT(
        VIDEO, "Pipeline cache failed validation: Incorrect device ID (file: {:#X}, device: {:#X})",
        header.device_id, g_vulkan_context->GetDeviceInfo().deviceID);
    return false;
  }

  if (std::memcmp(header.uuid, g_vulkan_context->GetDeviceInfo().pipelineCacheUUID, VK_UUID_SIZE) !=
      0)
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Incorrect UUID");
    return false;
  }

  return true;
}

void ObjectCache::DestroyPipelineCache()
{
  vkDestroyPipelineCache(g_vulkan_context->GetDevice(), m_pipeline_cache, nullptr);
  m_pipeline_cache = VK_NULL_HANDLE;
}

void ObjectCache::SavePipelineCache()
{
  size_t data_size;
  VkResult res =
      vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  // A single key of 1 with the whole pipeline cache, which the driver writes where the store
  // keeps it: written only when it changed.
  using Store = Common::LinearDiskCache<u32, u8>;
  u8* const store = static_cast<u8*>(std::malloc(Store::SingleSize(static_cast<u32>(data_size))));
  if (!store)
    return;
  res = vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size,
                               Store::SingleValue(store));
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    std::free(store);
    return;
  }
  Store::WriteSingle(m_pipeline_cache_filename, 1, store, static_cast<u32>(data_size));
  std::free(store);
}

void ObjectCache::ReloadPipelineCache()
{
  SavePipelineCache();

  if (g_ActiveConfig.bShaderCache)
    LoadPipelineCache();
  else
    CreatePipelineCache();
}
}  // namespace Vulkan
