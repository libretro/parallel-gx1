// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/EFBInterface.h"

namespace Null
{
class NullGfx final : public AbstractGfx
{
public:
  NullGfx();
  ~NullGfx();

  bool IsHeadless() const;
  bool SupportsUtilityDrawing() const;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name);
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config);
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments);

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
  SurfaceInfo GetSurfaceInfo() const { return {}; }
};

class NullEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};

}  // namespace Null
