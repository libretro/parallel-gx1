// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d11_4.h>
#include <string_view>

#include "VideoBackends/D3D/D3DState.h"
#include "VideoCommon/AbstractGfx.h"

class BoundingBox;
class FrameOutput;

namespace DX11
{
class DXTexture;
class DXFramebuffer;

class Gfx final : public ::AbstractGfx
{
public:
  Gfx(std::unique_ptr<FrameOutput> output, float backbuffer_scale);
  ~Gfx();

  StateCache& GetStateCache() { return m_state_cache; }

  bool IsHeadless() const;

  AbstractTexture* CreateTexture(const TextureConfig& config,
                                                 std::string_view name);
  AbstractStagingTexture*
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config);
  AbstractShader*
  CreateShaderFromSource(ShaderStage stage, ShaderCode* source,
                         std::string_view name);
  AbstractShader* CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name);
  NativeVertexFormat*
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl);
  AbstractPipeline* CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0);
  AbstractFramebuffer*
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments);

  void SetPipeline(const AbstractPipeline* pipeline);
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

  void Flush();
  void WaitForGPUIdle();

  SurfaceInfo GetSurfaceInfo() const;

  // Binds everything again after the frontend used the context.
  void RestoreContext();

private:
  StateCache m_state_cache;

  D3D11_VIEWPORT m_viewport = {};
  D3D11_RECT m_scissor = {};
  float m_backbuffer_scale;
  std::unique_ptr<FrameOutput> m_output;
};
}  // namespace DX11
