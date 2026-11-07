// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "VideoCommon/AbstractGfx.h"

namespace SW
{
class SWGfx final : public AbstractGfx
{
public:
  SWGfx();
  ~SWGfx();

  bool IsHeadless() const;
  bool SupportsUtilityDrawing() const;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name);
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config);
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments);

  bool BindBackbuffer(const ClearColor& clear_color = {});

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         std::string_view name);
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name);
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl);
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0);

  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc);

  void ScaleTexture(AbstractFramebuffer* dst_framebuffer, const MathUtil::Rectangle<int>& dst_rect,
                    const AbstractTexture* src_texture,
                    const MathUtil::Rectangle<int>& src_rect);

  void SetScissorRect(const MathUtil::Rectangle<int>& rc);

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z);

  SurfaceInfo GetSurfaceInfo() const;

private:
  std::vector<u32> m_picture;
};

}  // namespace SW
