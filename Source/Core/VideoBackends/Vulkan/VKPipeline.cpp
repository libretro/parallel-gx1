// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKPipeline.h"

#include <array>

#include <cstring>

#include <retro_atomic.h>

#include "Common/Assert.h"
#include "Common/EnumMap.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/StutterStats.h"

namespace Vulkan
{
/* Pipelines that differ only in what is set per draw are one VkPipeline,
 * found in an open-addressed table every thread building pipelines reads
 * and adds to without a lock. A slot goes from free to building to ready,
 * and to dead when the last pipeline using it goes: it is never free
 * again until the backend comes up again, so a search ends at the first
 * free slot. Its key is written once, before it is ready. */
#define SHARED_SLOTS 16384u
#define SHARED_PROBES 64u

enum
{
  SHARED_FREE = 0,
  SHARED_BUILDING,
  SHARED_READY,
  SHARED_DEAD
};

struct VKSharedKey
{
  const NativeVertexFormat* vertex_format;
  u32 vertex_shader;
  u32 geometry_shader;
  u32 pixel_shader;
  u32 rasterization;
  u32 blending;
  u32 framebuffer;
  u32 usage;
};

struct VKSharedPipeline
{
  VkPipeline pipeline;
  VKSharedKey key;
  u32 hash;
  retro_atomic_int_t state;
  retro_atomic_int_t refs;
};

static VKSharedPipeline s_shared[SHARED_SLOTS];

static u32 ShaderUid(const AbstractShader* shader)
{
  return shader ? static_cast<const VKShader*>(shader)->GetUid() : 0u;
}

static u32 HashMix(u32 h, u32 v)
{
  h ^= v;
  h *= 0x01000193u;
  return h ^ (h >> 15);
}

static u32 HashSharedKey(const VKSharedKey* key)
{
  const uintptr_t vf = reinterpret_cast<uintptr_t>(key->vertex_format);
  u32 h = 0x811c9dc5u;
  h = HashMix(h, static_cast<u32>(vf));
  h = HashMix(h, static_cast<u32>(static_cast<u64>(vf) >> 32));
  h = HashMix(h, key->vertex_shader);
  h = HashMix(h, key->geometry_shader);
  h = HashMix(h, key->pixel_shader);
  h = HashMix(h, key->rasterization);
  h = HashMix(h, key->blending);
  h = HashMix(h, key->framebuffer);
  return HashMix(h, key->usage);
}

static bool SharedKeyEqual(const VKSharedKey* a, const VKSharedKey* b)
{
  return a->vertex_format == b->vertex_format && a->vertex_shader == b->vertex_shader &&
         a->geometry_shader == b->geometry_shader && a->pixel_shader == b->pixel_shader &&
         a->rasterization == b->rasterization && a->blending == b->blending &&
         a->framebuffer == b->framebuffer && a->usage == b->usage;
}

/* A ready slot with the key, its count raised: one whose last user is
 * going is passed over, as its count is never raised from zero. */
static VKSharedPipeline* SharedFind(const VKSharedKey* key, u32 hash)
{
  u32 i;
  for (i = 0; i < SHARED_PROBES; i++)
  {
    VKSharedPipeline* s = &s_shared[(hash + i) & (SHARED_SLOTS - 1)];
    const int state = retro_atomic_load_acquire_int(&s->state);
    if (state == SHARED_FREE)
      return nullptr;
    if (state != SHARED_READY || s->hash != hash || !SharedKeyEqual(&s->key, key))
      continue;
    for (;;)
    {
      const int refs = retro_atomic_load_acquire_int(&s->refs);
      if (refs <= 0)
        break;
      if (retro_atomic_cas_int(&s->refs, refs, refs + 1))
        return s;
    }
  }
  return nullptr;
}

/* The first free slot on the key's chain, or none: the pipeline is then
 * its own. */
static VKSharedPipeline* SharedInsert(const VKSharedKey* key, u32 hash, VkPipeline pipeline)
{
  u32 i;
  for (i = 0; i < SHARED_PROBES; i++)
  {
    VKSharedPipeline* s = &s_shared[(hash + i) & (SHARED_SLOTS - 1)];
    if (retro_atomic_load_acquire_int(&s->state) != SHARED_FREE ||
        !retro_atomic_cas_int(&s->state, SHARED_FREE, SHARED_BUILDING))
    {
      continue;
    }
    s->key = *key;
    s->hash = hash;
    s->pipeline = pipeline;
    retro_atomic_store_relaxed_int(&s->refs, 1);
    retro_atomic_store_release_int(&s->state, SHARED_READY);
    return s;
  }
  return nullptr;
}

void VKPipeline::ResetSharedPipelines()
{
  u32 i;
  for (i = 0; i < SHARED_SLOTS; i++)
  {
    retro_atomic_store_relaxed_int(&s_shared[i].refs, 0);
    retro_atomic_store_relaxed_int(&s_shared[i].state, SHARED_FREE);
  }
  retro_atomic_thread_fence_release();
}

/* Pipeline libraries, of the four parts a pipeline is linked from, are
 * kept in the same table, their key's usage carrying which part they are:
 * they stay, counted once, until DestroySharedLibraries. */
#define SHARED_LIBRARY_SHIFT 8

void VKPipeline::DestroySharedLibraries()
{
  u32 i;
  for (i = 0; i < SHARED_SLOTS; i++)
  {
    VKSharedPipeline* s = &s_shared[i];
    if (retro_atomic_load_acquire_int(&s->state) != SHARED_READY ||
        !(s->key.usage >> SHARED_LIBRARY_SHIFT))
    {
      continue;
    }
    vkDestroyPipeline(g_vulkan_context->GetDevice(), s->pipeline, nullptr);
    retro_atomic_store_relaxed_int(&s->refs, 0);
    retro_atomic_store_release_int(&s->state, SHARED_DEAD);
  }
}

/* The library for one part, found or built from the parts of info it
 * takes; *owned when the table had no room for it, and the caller
 * destroys it once linked. */
static VkPipeline GetPipelineLibrary(const VKSharedKey* key,
                                     const VkGraphicsPipelineCreateInfo* info,
                                     VkGraphicsPipelineLibraryFlagsEXT part, bool* owned)
{
  const u32 hash = HashSharedKey(key);
  VKSharedPipeline* s = SharedFind(key, hash);
  VkGraphicsPipelineLibraryCreateInfoEXT library_info;
  VkGraphicsPipelineCreateInfo library_create;
  VkPipeline library;

  *owned = false;
  if (s)
  {
    library = s->pipeline;
    retro_atomic_fetch_sub_int(&s->refs, 1);
    return library;
  }
  library_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
  library_info.pNext = info->pNext;
  library_info.flags = part;
  library_create = *info;
  library_create.pNext = &library_info;
  library_create.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
  if (vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), g_object_cache->GetPipelineCache(),
                                1, &library_create, nullptr, &library) != VK_SUCCESS)
  {
    return VK_NULL_HANDLE;
  }
  if (!SharedInsert(key, hash, library))
    *owned = true;
  return library;
}

/* A pipeline linked from the libraries of its four parts, each found or
 * built with only what that part takes: the vertex input, the vertex and
 * geometry shaders, the pixel shader, and the colour output. With cull
 * mode, depth state and blending set per draw, the parts are shared by
 * most of the pipelines asked for, and linking without link-time
 * optimisation costs a fraction of building the pipeline. */
static VkPipeline LinkPipeline(const AbstractPipelineConfig& build, const VKSharedKey* key,
                               const VkGraphicsPipelineCreateInfo* info)
{
  static const VkGraphicsPipelineLibraryFlagsEXT parts[4] = {
      VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
      VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
      VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
      VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
  };
  VkGraphicsPipelineCreateInfo part_info[4];
  VKSharedKey part_key[4];
  VkPipeline libraries[4];
  bool owned[4];
  VkPipelineLibraryCreateInfoKHR link_info;
  VkGraphicsPipelineCreateInfo link;
  VkPipeline pipeline = VK_NULL_HANDLE;
  u32 i;

  for (i = 0; i < 4; i++)
  {
    part_info[i] = *info;
    part_info[i].stageCount = 0;
    part_info[i].pStages = nullptr;
    part_info[i].pVertexInputState = nullptr;
    part_info[i].pInputAssemblyState = nullptr;
    part_info[i].pViewportState = nullptr;
    part_info[i].pRasterizationState = nullptr;
    part_info[i].pMultisampleState = nullptr;
    part_info[i].pDepthStencilState = nullptr;
    part_info[i].pColorBlendState = nullptr;
    memset(&part_key[i], 0, sizeof(part_key[i]));
    part_key[i].usage = key->usage | ((i + 1) << SHARED_LIBRARY_SHIFT);
  }
  /* vertex input: the vertex layout and the topology */
  part_info[0].pVertexInputState = info->pVertexInputState;
  part_info[0].pInputAssemblyState = info->pInputAssemblyState;
  part_info[0].layout = VK_NULL_HANDLE;
  part_info[0].renderPass = VK_NULL_HANDLE;
  part_info[0].pDynamicState = nullptr;
  part_key[0].vertex_format = key->vertex_format;
  part_key[0].rasterization = static_cast<u32>(build.rasterization_state.primitive.Value());
  /* the vertex and geometry shaders, all stages but the last */
  part_info[1].stageCount = info->stageCount - 1;
  part_info[1].pStages = info->pStages;
  part_info[1].pViewportState = info->pViewportState;
  part_info[1].pRasterizationState = info->pRasterizationState;
  part_key[1].vertex_shader = key->vertex_shader;
  part_key[1].geometry_shader = key->geometry_shader;
  part_key[1].rasterization = key->rasterization;
  part_key[1].framebuffer = key->framebuffer;
  /* the pixel shader */
  part_info[2].stageCount = 1;
  part_info[2].pStages = info->pStages + info->stageCount - 1;
  part_info[2].pMultisampleState = info->pMultisampleState;
  part_info[2].pDepthStencilState = info->pDepthStencilState;
  part_key[2].pixel_shader = key->pixel_shader;
  part_key[2].framebuffer = key->framebuffer;
  /* the colour output */
  part_info[3].pMultisampleState = info->pMultisampleState;
  part_info[3].pColorBlendState = info->pColorBlendState;
  part_info[3].layout = VK_NULL_HANDLE;
  part_key[3].blending = key->blending;
  part_key[3].framebuffer = key->framebuffer;

  for (i = 0; i < 4; i++)
  {
    libraries[i] = GetPipelineLibrary(&part_key[i], &part_info[i], parts[i], &owned[i]);
    if (libraries[i] == VK_NULL_HANDLE)
      break;
  }
  if (i == 4)
  {
    link_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    link_info.pNext = nullptr;
    link_info.libraryCount = 4;
    link_info.pLibraries = libraries;
    memset(&link, 0, sizeof(link));
    link.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    link.pNext = &link_info;
    link.layout = info->layout;
    link.basePipelineIndex = -1;
    if (vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(),
                                  g_object_cache->GetPipelineCache(), 1, &link, nullptr,
                                  &pipeline) != VK_SUCCESS)
    {
      pipeline = VK_NULL_HANDLE;
    }
  }
  while (i--)
  {
    if (owned[i])
      vkDestroyPipeline(g_vulkan_context->GetDevice(), libraries[i], nullptr);
  }
  return pipeline;
}

VKPipeline::VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                       VkPipelineLayout pipeline_layout, AbstractPipelineUsage usage,
                       u32 dynamic_state, u32 dynamic_blend, VKSharedPipeline* shared)
    : AbstractPipeline(config), m_pipeline(pipeline), m_pipeline_layout(pipeline_layout),
      m_shared(shared), m_usage(usage), m_dynamic_state(dynamic_state),
      m_dynamic_blend(dynamic_blend)
{
}

VKPipeline::~VKPipeline()
{
  if (m_shared)
  {
    if (retro_atomic_fetch_sub_int(&m_shared->refs, 1) != 1)
      return;
    retro_atomic_store_release_int(&m_shared->state, SHARED_DEAD);
  }
  vkDestroyPipeline(g_vulkan_context->GetDevice(), m_pipeline, nullptr);
}

static bool IsStripPrimitiveTopology(VkPrimitiveTopology topology)
{
  return topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
         topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ||
         topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

static const VkCullModeFlags s_cull_modes[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT,
                                                VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_FRONT_AND_BACK};

static VkPipelineRasterizationStateCreateInfo
GetVulkanRasterizationState(const RasterizationState& state)
{
  bool depth_clamp = g_backend_info.bSupportsDepthClamp;

  return {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,               // const void*                               pNext
      0,                     // VkPipelineRasterizationStateCreateFlags   flags
      depth_clamp,           // VkBool32                                  depthClampEnable
      VK_FALSE,              // VkBool32                                  rasterizerDiscardEnable
      VK_POLYGON_MODE_FILL,  // VkPolygonMode                             polygonMode
      s_cull_modes[u32(state.cull_mode.Value())],  // VkCullModeFlags     cullMode
      VK_FRONT_FACE_CLOCKWISE,                   // VkFrontFace            frontFace
      VK_FALSE,  // VkBool32                                              depthBiasEnable
      0.0f,      // float                                                 depthBiasConstantFactor
      0.0f,      // float                                                 depthBiasClamp
      0.0f,      // float                                                 depthBiasSlopeFactor
      1.0f       // float                                                 lineWidth
  };
}

static VkPipelineMultisampleStateCreateInfo GetVulkanMultisampleState(const FramebufferState& state)
{
  return {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,  // const void*                              pNext
      0,        // VkPipelineMultisampleStateCreateFlags    flags
      static_cast<VkSampleCountFlagBits>(
          state.samples.Value()),  // VkSampleCountFlagBits                    rasterizationSamples
      static_cast<bool>(state.per_sample_shading),  // VkBool32 sampleShadingEnable
      1.0f,      // float                                    minSampleShading
      nullptr,   // const VkSampleMask*                      pSampleMask;
      VK_FALSE,  // VkBool32                                 alphaToCoverageEnable
      VK_FALSE   // VkBool32                                 alphaToOneEnable
  };
}

static VkCompareOp GetVulkanCompareOp(CompareMode func)
{
  // Less/greater are swapped due to inverted depth.
  const bool inverted_depth = !g_backend_info.bSupportsReversedDepthRange;
  switch (func)
  {
  case CompareMode::Never:
    return VK_COMPARE_OP_NEVER;
  case CompareMode::Less:
    return inverted_depth ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
  case CompareMode::Equal:
    return VK_COMPARE_OP_EQUAL;
  case CompareMode::LEqual:
    return inverted_depth ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  case CompareMode::Greater:
    return inverted_depth ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER;
  case CompareMode::NEqual:
    return VK_COMPARE_OP_NOT_EQUAL;
  case CompareMode::GEqual:
    return inverted_depth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
  case CompareMode::Always:
    return VK_COMPARE_OP_ALWAYS;
  default:
    PanicAlertFmt("Invalid compare mode {}", func);
    return VK_COMPARE_OP_ALWAYS;
  }
}

static VkPipelineDepthStencilStateCreateInfo GetVulkanDepthStencilState(const DepthState& state)
{
  const VkCompareOp compare_op = GetVulkanCompareOp(state.func);

  return {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,              // const void*                               pNext
      0,                    // VkPipelineDepthStencilStateCreateFlags    flags
      state.test_enable,    // VkBool32                                  depthTestEnable
      state.update_enable,  // VkBool32                                  depthWriteEnable
      compare_op,           // VkCompareOp                               depthCompareOp
      VK_FALSE,             // VkBool32                                  depthBoundsTestEnable
      VK_FALSE,             // VkBool32                                  stencilTestEnable
      {},                   // VkStencilOpState                          front
      {},                   // VkStencilOpState                          back
      0.0f,                 // float                                     minDepthBounds
      1.0f                  // float                                     maxDepthBounds
  };
}

static VkPipelineColorBlendAttachmentState
GetVulkanAttachmentBlendState(const BlendingState& state, AbstractPipelineUsage usage)
{
  VkPipelineColorBlendAttachmentState vk_state = {};

  bool use_dual_source = state.use_dual_src;

  vk_state.blendEnable = static_cast<VkBool32>(state.blend_enable);
  vk_state.colorBlendOp = state.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  vk_state.alphaBlendOp = state.subtract_alpha ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;

  if (use_dual_source)
  {
    static constexpr Common::EnumMap<VkBlendFactor, SrcBlendFactor::InvDstAlpha> src_factors{
        VK_BLEND_FACTOR_ZERO,       VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_DST_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };
    static constexpr Common::EnumMap<VkBlendFactor, DstBlendFactor::InvDstAlpha> dst_factors{
        VK_BLEND_FACTOR_ZERO,       VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    vk_state.srcColorBlendFactor = src_factors[state.src_factor];
    vk_state.srcAlphaBlendFactor = src_factors[state.src_factor_alpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dst_factor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dst_factor_alpha];
  }
  else
  {
    static constexpr Common::EnumMap<VkBlendFactor, SrcBlendFactor::InvDstAlpha> src_factors{
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    static constexpr Common::EnumMap<VkBlendFactor, DstBlendFactor::InvDstAlpha> dst_factors{
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    vk_state.srcColorBlendFactor = src_factors[state.src_factor];
    vk_state.srcAlphaBlendFactor = src_factors[state.src_factor_alpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dst_factor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dst_factor_alpha];
  }

  if (state.color_update)
  {
    vk_state.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  }
  else
  {
    vk_state.colorWriteMask = 0;
  }

  if (state.alpha_update)
    vk_state.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

  return vk_state;
}

void VKPipeline::GetDynamicBlendState(u32 dynamic_blend, VkBool32* enable,
                                      VkColorBlendEquationEXT* equation,
                                      VkColorComponentFlags* write_mask)
{
  BlendingState state;
  state.hex = dynamic_blend & (VK_DYNAMIC_BLEND_MASK | VK_DYNAMIC_BLEND_DUAL_SRC);
  const VkPipelineColorBlendAttachmentState vk_state =
      GetVulkanAttachmentBlendState(state, AbstractPipelineUsage::GX);
  *enable = vk_state.blendEnable;
  equation->srcColorBlendFactor = vk_state.srcColorBlendFactor;
  equation->dstColorBlendFactor = vk_state.dstColorBlendFactor;
  equation->colorBlendOp = vk_state.colorBlendOp;
  equation->srcAlphaBlendFactor = vk_state.srcAlphaBlendFactor;
  equation->dstAlphaBlendFactor = vk_state.dstAlphaBlendFactor;
  equation->alphaBlendOp = vk_state.alphaBlendOp;
  *write_mask = vk_state.colorWriteMask;
}

VkLogicOp VKPipeline::GetVulkanLogicOp(u32 logic_mode)
{
  static const VkLogicOp vk_logic_ops[16] = {
      VK_LOGIC_OP_CLEAR,         VK_LOGIC_OP_AND,          VK_LOGIC_OP_AND_REVERSE,
      VK_LOGIC_OP_COPY,          VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_NO_OP,
      VK_LOGIC_OP_XOR,           VK_LOGIC_OP_OR,           VK_LOGIC_OP_NOR,
      VK_LOGIC_OP_EQUIVALENT,    VK_LOGIC_OP_INVERT,       VK_LOGIC_OP_OR_REVERSE,
      VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_OR_INVERTED,  VK_LOGIC_OP_NAND,
      VK_LOGIC_OP_SET};
  return vk_logic_ops[logic_mode & 15];
}

void VKPipeline::GetDrawState(const RasterizationState& rasterization, const DepthState& depth,
                              const BlendingState& blending, u32* state, u32* blend)
{
  *state = 0;
  *blend = 0;
  if (!g_vulkan_context->SupportsExtendedDynamicState())
    return;
  *state = VK_DYNAMIC_USED | s_cull_modes[u32(rasterization.cull_mode.Value())] |
           (depth.test_enable ? VK_DYNAMIC_DEPTH_TEST : 0u) |
           (depth.update_enable ? VK_DYNAMIC_DEPTH_WRITE : 0u) |
           (static_cast<u32>(GetVulkanCompareOp(depth.func)) << VK_DYNAMIC_DEPTH_OP_SHIFT);
  if (!g_vulkan_context->SupportsDynamicBlending())
    return;
  *blend = VK_DYNAMIC_BLEND_USED |
           (blending.hex & (VK_DYNAMIC_BLEND_MASK | VK_DYNAMIC_BLEND_DUAL_SRC));
  if (g_vulkan_context->SupportsDynamicLogicOp())
    *blend |= VK_DYNAMIC_BLEND_LOGIC_USED | (blending.hex & VK_DYNAMIC_BLEND_LOGIC_MASK);
}

void VKPipeline::PopulateBackendInfoDrawState(BackendInfo* backend_info)
{
  RasterizationState cull;
  DepthState depth;
  cull.hex = 0;
  cull.cull_mode = static_cast<CullMode>(3);
  depth.hex = 0;
  depth.test_enable = 1;
  depth.update_enable = 1;
  depth.func = static_cast<CompareMode>(7);
  backend_info->uDrawStateRasterizationBits = 0;
  backend_info->uDrawStateDepthBits = 0;
  backend_info->uDrawStateBlendingBits = 0;
  if (!g_vulkan_context->SupportsExtendedDynamicState())
    return;
  backend_info->uDrawStateRasterizationBits = cull.hex;
  backend_info->uDrawStateDepthBits = depth.hex;
  if (!g_vulkan_context->SupportsDynamicBlending())
    return;
  /* use_dual_src only picks the factors, which are set per draw too: the pipeline is the same */
  backend_info->uDrawStateBlendingBits = VK_DYNAMIC_BLEND_MASK | VK_DYNAMIC_BLEND_DUAL_SRC;
  if (g_vulkan_context->SupportsDynamicLogicOp())
    backend_info->uDrawStateBlendingBits |= VK_DYNAMIC_BLEND_LOGIC_MASK;
}

static VkPipelineColorBlendStateCreateInfo
GetVulkanColorBlendState(const BlendingState& state,
                         const VkPipelineColorBlendAttachmentState* attachments,
                         uint32_t num_attachments)
{
  VkBool32 vk_logic_op_enable = static_cast<VkBool32>(state.logic_op_enable);
  if (vk_logic_op_enable && !g_backend_info.bSupportsLogicOp)
  {
    // At the time of writing, Adreno and Mali drivers didn't support logic ops.
    // The "emulation" through blending path has been removed, so just disable it completely.
    // These drivers don't support dual-source blend either, so issues are to be expected.
    vk_logic_op_enable = VK_FALSE;
  }

  VkLogicOp vk_logic_op =
      vk_logic_op_enable ? VKPipeline::GetVulkanLogicOp(u32(state.logic_mode.Value())) :
                           VK_LOGIC_OP_CLEAR;

  VkPipelineColorBlendStateCreateInfo vk_state = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,                  // const void*                                   pNext
      0,                        // VkPipelineColorBlendStateCreateFlags          flags
      vk_logic_op_enable,       // VkBool32                                      logicOpEnable
      vk_logic_op,              // VkLogicOp                                     logicOp
      num_attachments,          // uint32_t                                      attachmentCount
      attachments,              // const VkPipelineColorBlendAttachmentState*    pAttachments
      {1.0f, 1.0f, 1.0f, 1.0f}  // float                                         blendConstants[4]
  };

  return vk_state;
}

VKPipeline* VKPipeline::Create(const AbstractPipelineConfig& config)
{
  DEBUG_ASSERT(config.vertex_shader && config.pixel_shader);

  // Get render pass for config.
  VkRenderPass render_pass = g_object_cache->GetRenderPass(
      VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.color_texture_format),
      VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.depth_texture_format),
      config.framebuffer_state.samples, VK_ATTACHMENT_LOAD_OP_LOAD);

  if (render_pass == VK_NULL_HANDLE)
  {
    PanicAlertFmt("Failed to get render pass");
    return nullptr;
  }

  // Get pipeline layout.
  VkPipelineLayout pipeline_layout;
  switch (config.usage)
  {
  case AbstractPipelineUsage::GX:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_STANDARD);
    break;
  case AbstractPipelineUsage::GXUber:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UBER);
    break;
  case AbstractPipelineUsage::Utility:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UTILITY);
    break;
  default:
    PanicAlertFmt("Unknown pipeline layout.");
    return nullptr;
  }

  /* Cull mode and depth state set per draw, and blending with them where
   * the device allows it: the pipeline is built, and shared, with them
   * left out of its key. */
  AbstractPipelineConfig build = config;
  u32 dynamic_bits = 0;
  u32 dynamic_blend = 0;
  VKSharedKey key;
  u32 hash = 0;
  if (config.usage != AbstractPipelineUsage::Utility &&
      g_vulkan_context->SupportsExtendedDynamicState())
  {
    VKSharedPipeline* shared;
    /* the logic op mode alone picks no pipeline either, where it is set
     * per draw: one with integer output draws every mode */
    GetDrawState(config.rasterization_state, config.depth_state, config.blending_state,
                 &dynamic_bits, &dynamic_blend);
    build.rasterization_state.hex &= ~g_backend_info.uDrawStateRasterizationBits;
    build.depth_state.hex &= ~g_backend_info.uDrawStateDepthBits;
    build.blending_state.hex &= ~g_backend_info.uDrawStateBlendingBits;
    key.vertex_format = build.vertex_format;
    key.vertex_shader = ShaderUid(build.vertex_shader);
    key.geometry_shader = ShaderUid(build.geometry_shader);
    key.pixel_shader = ShaderUid(build.pixel_shader);
    key.rasterization = build.rasterization_state.hex;
    key.blending = build.blending_state.hex;
    key.framebuffer = build.framebuffer_state.hex;
    key.usage = static_cast<u32>(build.usage);
    hash = HashSharedKey(&key);
    shared = SharedFind(&key, hash);
    if (shared)
    {
      return new VKPipeline(config, shared->pipeline, pipeline_layout, config.usage,
                                          dynamic_bits, dynamic_blend, shared);
    }
  }

  // Declare descriptors for empty vertex buffers/attributes
  static const VkPipelineVertexInputStateCreateInfo empty_vertex_input_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,  // const void*                                pNext
      0,        // VkPipelineVertexInputStateCreateFlags       flags
      0,        // uint32_t                                    vertexBindingDescriptionCount
      nullptr,  // const VkVertexInputBindingDescription*      pVertexBindingDescriptions
      0,        // uint32_t                                    vertexAttributeDescriptionCount
      nullptr   // const VkVertexInputAttributeDescription*    pVertexAttributeDescriptions
  };

  // Vertex inputs
  const VkPipelineVertexInputStateCreateInfo& vertex_input_state =
      config.vertex_format ?
          static_cast<const VertexFormat*>(config.vertex_format)->GetVertexInputStateInfo() :
          empty_vertex_input_state;

  // Input assembly
  static constexpr std::array<VkPrimitiveTopology, 4> vk_primitive_topologies = {
      {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP}};
  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
      vk_primitive_topologies[static_cast<u32>(build.rasterization_state.primitive.Value())],
      VK_FALSE};

  // See Vulkan spec, section 19:
  // If topology is VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
  // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
  // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY or VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
  // primitiveRestartEnable must be VK_FALSE
  if (g_backend_info.bSupportsPrimitiveRestart &&
      IsStripPrimitiveTopology(input_assembly_state.topology))
  {
    input_assembly_state.primitiveRestartEnable = VK_TRUE;
  }

  // Shaders to stages
  VkPipelineShaderStageCreateInfo shader_stages[3];
  uint32_t num_shader_stages = 0;
  if (config.vertex_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_VERTEX_BIT,
        static_cast<const VKShader*>(config.vertex_shader)->GetShaderModule(),
        "main"};
  }
  if (config.geometry_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_GEOMETRY_BIT,
        static_cast<const VKShader*>(config.geometry_shader)->GetShaderModule(),
        "main"};
  }
  if (config.pixel_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        static_cast<const VKShader*>(config.pixel_shader)->GetShaderModule(),
        "main"};
  }

  // Fill in Vulkan descriptor structs from our state structures.
  VkPipelineRasterizationStateCreateInfo rasterization_state =
      GetVulkanRasterizationState(build.rasterization_state);
  VkPipelineMultisampleStateCreateInfo multisample_state =
      GetVulkanMultisampleState(config.framebuffer_state);
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state =
      GetVulkanDepthStencilState(build.depth_state);
  VkPipelineColorBlendAttachmentState blend_attachment_state =
      GetVulkanAttachmentBlendState(build.blending_state, config.usage);

  VkPipelineColorBlendStateCreateInfo blend_state =
      GetVulkanColorBlendState(build.blending_state, &blend_attachment_state, 1);

  static const VkDepthClampRangeEXT clamp_range = {0.0f, MAX_EFB_DEPTH};
  static const VkPipelineViewportDepthClampControlCreateInfoEXT depth_clamp_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT, nullptr,
      VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT,  // VkDepthClampModeEXT            depthClampMode
      &clamp_range  // const VkDepthClampRangeEXT*    pDepthClampRange
  };

  // This viewport isn't used, but needs to be specified anyway.
  static const VkViewport viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  static const VkRect2D scissor = {{0, 0}, {1, 1}};
  static const VkPipelineViewportStateCreateInfo viewport_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      g_backend_info.bSupportsUnrestrictedDepthRange ? &depth_clamp_state : nullptr,
      0,          // VkPipelineViewportStateCreateFlags    flags;
      1,          // uint32_t                              viewportCount
      &viewport,  // const VkViewport*                     pViewports
      1,          // uint32_t                              scissorCount
      &scissor    // const VkRect2D*                       pScissors
  };

  // Viewport and scissor are set per draw, and cull mode and depth state with them where the
  // device allows it.
  static const VkDynamicState dynamic_states[11] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_CULL_MODE_EXT,
      VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
      VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
      VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
      VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
      VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
      VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
      VK_DYNAMIC_STATE_LOGIC_OP_EXT,
  };
  const VkPipelineDynamicStateCreateInfo dynamic_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr,
      0,  // VkPipelineDynamicStateCreateFlags    flags
      (dynamic_blend & VK_DYNAMIC_BLEND_LOGIC_USED) ? 11u :
      dynamic_blend                                 ? 9u :
      dynamic_bits                                  ? 6u :
                                                      2u,  // uint32_t dynamicStateCount
      dynamic_states  // const VkDynamicState*                pDynamicStates
  };

  // Combine to full pipeline info structure.
  VkGraphicsPipelineCreateInfo pipeline_info = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      nullptr,                // VkStructureType sType
      0,                      // VkPipelineCreateFlags                            flags
      num_shader_stages,      // uint32_t                                         stageCount
      shader_stages,          // const VkPipelineShaderStageCreateInfo*           pStages
      &vertex_input_state,    // const VkPipelineVertexInputStateCreateInfo*      pVertexInputState
      &input_assembly_state,  // const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState
      nullptr,                // const VkPipelineTessellationStateCreateInfo*     pTessellationState
      &viewport_state,        // const VkPipelineViewportStateCreateInfo*         pViewportState
      &rasterization_state,  // const VkPipelineRasterizationStateCreateInfo*    pRasterizationState
      &multisample_state,    // const VkPipelineMultisampleStateCreateInfo*      pMultisampleState
      &depth_stencil_state,  // const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState
      &blend_state,          // const VkPipelineColorBlendStateCreateInfo*       pColorBlendState
      &dynamic_state,        // const VkPipelineDynamicStateCreateInfo*          pDynamicState
      pipeline_layout,       // VkPipelineLayout                                 layout
      render_pass,           // VkRenderPass                                     renderPass
      0,                     // uint32_t                                         subpass
      VK_NULL_HANDLE,        // VkPipeline                                       basePipelineHandle
      -1                     // int32_t                                          basePipelineIndex
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  STUTTER_MARK(t);
  /* Ubershader pipelines, built on the spot when a draw needs one, are
   * linked from libraries where the device can; a link that fails builds
   * the pipeline whole. */
  if (config.usage == AbstractPipelineUsage::GXUber && dynamic_bits &&
      g_vulkan_context->SupportsFastLinking())
  {
    pipeline = LinkPipeline(build, &key, &pipeline_info);
  }
  if (pipeline == VK_NULL_HANDLE)
  {
    VkResult res =
        vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), g_object_cache->GetPipelineCache(),
                                  1, &pipeline_info, nullptr, &pipeline);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines failed: ");
      return VK_NULL_HANDLE;
    }
  }
  STUTTER_DRIVER_SINCE(t);

  return new VKPipeline(config, pipeline, pipeline_layout, config.usage,
                                      dynamic_bits, dynamic_blend,
                                      dynamic_bits ? SharedInsert(&key, hash, pipeline) : nullptr);
}
}  // namespace Vulkan
