// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Null/NullGfx.h"

#include "VideoBackends/Null/NullTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoBackendKind.h"

namespace Null
{
// Init functions
NullGfx::NullGfx()
{
  g_video_backend_kind = VIDEO_BACKEND_NULL;
  UpdateActiveConfig();
}

NullGfx::~NullGfx()
{
  UpdateActiveConfig();
}

bool NullGfx::IsHeadless() const
{
  return true;
}

bool NullGfx::SupportsUtilityDrawing() const
{
  return false;
}

AbstractTexture* NullGfx::CreateTexture(const TextureConfig& config,
                                                        std::string_view /* name */)
{
  return new NullTexture(config);
}

AbstractStagingTexture* NullGfx::CreateStagingTexture(StagingTextureType type,
                                                                      const TextureConfig& config)
{
  return new NullStagingTexture(type, config);
}

AbstractShader*
NullGfx::CreateShaderFromSource(ShaderStage stage, ShaderCode* /* source */,
                                std::string_view /* name */)
{
  return new AbstractShader(stage);
}

AbstractShader* NullGfx::CreateShaderFromBinary(ShaderStage stage,
                                                                const void* /* data */,
                                                                size_t /* length */,
                                                                std::string_view /* name */)
{
  return new AbstractShader(stage);
}

AbstractPipeline* NullGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                          const void* cache_data,
                                                          size_t cache_data_length)
{
  return new AbstractPipeline();
}

AbstractFramebuffer*
NullGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment)
{
  return NullFramebuffer::Create(static_cast<NullTexture*>(color_attachment),
                                 static_cast<NullTexture*>(depth_attachment));
}

NativeVertexFormat*
NullGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return new NativeVertexFormat(vtx_decl);
}

}  // namespace Null
