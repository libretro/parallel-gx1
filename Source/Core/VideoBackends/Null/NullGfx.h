// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractGfx.h"

namespace Null
{
class NullGfx final : public AbstractGfx
{
public:
  NullGfx();
  ~NullGfx();

  bool IsHeadless() const;
  bool SupportsUtilityDrawing() const;

  AbstractTexture* CreateTexture(const TextureConfig& config,
                                                 std::string_view name);
  AbstractStagingTexture*
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config);
  AbstractFramebuffer*
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment);

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
  SurfaceInfo GetSurfaceInfo() const { return {}; }
};

}  // namespace Null
