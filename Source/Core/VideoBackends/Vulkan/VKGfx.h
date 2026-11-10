// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"

class FrameOutput;

namespace Vulkan
{
class StagingTexture2D;
class VKFramebuffer;
class VKPipeline;
class VKTexture;

class VKGfx final : public ::AbstractGfx
{
public:
  VKGfx(FrameOutput* output, float backbuffer_scale);
  ~VKGfx();

  static VKGfx* GetInstance() { return static_cast<VKGfx*>(g_gfx); }

  bool IsHeadless() const;

  AbstractTexture* CreateTexture(const TextureConfig& config,
                                                 std::string_view name);
  AbstractStagingTexture*
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config);
  AbstractFramebuffer*
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment);

  AbstractShader*
  CreateShaderFromSource(ShaderStage stage, ShaderCode* source,
                         std::string_view name);
  AbstractShader* CreateShaderFromBuild(ShaderStage stage, shader_build_fn fn, const void* arg,
                                        std::string_view name);
  AbstractShader* CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name);
  NativeVertexFormat*
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl);
  AbstractPipeline* CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0);


  void Flush();
  void WaitForGPUIdle();
  void OnConfigChanged(u32 bits);

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z);

  void SetPipeline(const AbstractPipeline* pipeline);
  bool DecodeTextureAhead(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                          u32 groups_x, u32 groups_y, AbstractTexture* scratch,
                          AbstractTexture* dst, const MathUtil::Rectangle<int>& rect,
                          u32 dst_level);
  void SetDrawState(const RasterizationState& rasterization, const DepthState& depth,
                    const BlendingState& blending);
  void SetFramebuffer(AbstractFramebuffer* framebuffer);
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer);
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f);
  void SetScissorRect(const MathUtil::Rectangle<int>& rc);
  void SetTexture(u32 index, const AbstractTexture* texture);
  void SetSamplerState(u32 index, const SamplerState& state);
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write);
  void UnbindTexture(const AbstractTexture* texture);
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth);
  void Draw(u32 base_vertex, u32 num_vertices);
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex);
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z);
  bool BindBackbuffer(const ClearColor& clear_color = {});
  void PresentBackbuffer();
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height);
  bool IsTextureHeld(const AbstractTexture* texture);

  SurfaceInfo GetSurfaceInfo() const;

  // Completes the current render pass, executes the command buffer, and restores state ready for
  // next render. Use when you want to kick the current buffer to make room for new data.
  void ExecuteCommandBuffer(bool execute_off_thread, bool wait_for_completion = false);

private:

  void ResetSamplerStates();

  void BindFramebuffer(VKFramebuffer* fb);

  FrameOutput* m_output;  // owned
  float m_backbuffer_scale;

  // Keep a copy of sampler states to avoid cache lookups every draw
  std::array<SamplerState, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_sampler_states = {};
};
}  // namespace Vulkan
