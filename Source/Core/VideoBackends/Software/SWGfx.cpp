// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Software/SWGfx.h"

#include <algorithm>

#ifdef __LIBRETRO__
#include "DolphinLibretro/Common/Globals.h"
#endif

#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/SWTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoCommon.h"

namespace SW
{
SWGfx::SWGfx() = default;

SWGfx::~SWGfx() = default;

bool SWGfx::IsHeadless() const
{
  return false;
}

bool SWGfx::SupportsUtilityDrawing() const
{
  return false;
}

std::unique_ptr<AbstractTexture> SWGfx::CreateTexture(const TextureConfig& config,
                                                      std::string_view /* name */)
{
  return std::make_unique<SWTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> SWGfx::CreateStagingTexture(StagingTextureType type,
                                                                    const TextureConfig& config)
{
  return std::make_unique<SWStagingTexture>(type, config);
}

std::unique_ptr<AbstractFramebuffer>
SWGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  return SWFramebuffer::Create(static_cast<SWTexture*>(color_attachment),
                               static_cast<SWTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

bool SWGfx::BindBackbuffer(const ClearColor& clear_color)
{
  return true;
}

class SWShader final : public AbstractShader
{
public:
  explicit SWShader(ShaderStage stage) : AbstractShader(stage) {}
  ~SWShader() override = default;

  BinaryData GetBinary() const override { return {}; }
};

std::unique_ptr<AbstractShader>
SWGfx::CreateShaderFromSource(ShaderStage stage, std::string_view /* source */,
                              std::string_view /* name */)
{
  return std::make_unique<SWShader>(stage);
}

std::unique_ptr<AbstractShader> SWGfx::CreateShaderFromBinary(ShaderStage stage,
                                                              const void* /* data */,
                                                              size_t /* length */,
                                                              std::string_view /* name */)
{
  return std::make_unique<SWShader>(stage);
}

class SWPipeline final : public AbstractPipeline
{
public:
  SWPipeline() = default;
  ~SWPipeline() override = default;
};

std::unique_ptr<AbstractPipeline> SWGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                        const void* cache_data,
                                                        size_t cache_data_length)
{
  return std::make_unique<SWPipeline>();
}

// Called on the GPU thread. The frontend takes the picture in memory, in
// its XRGB8888 order: one pass from the texture's RGBA8, into a buffer
// that grows only when the picture does.
void SWGfx::ShowImage(const AbstractTexture* source_texture,
                      const MathUtil::Rectangle<int>& source_rc)
{
  const SWTexture* texture = static_cast<const SWTexture*>(source_texture);
  const u32 tex_width = texture->GetWidth();
  const u32 tex_height = texture->GetHeight();
  const u32 left = static_cast<u32>(std::clamp(source_rc.left, 0, static_cast<int>(tex_width)));
  const u32 top = static_cast<u32>(std::clamp(source_rc.top, 0, static_cast<int>(tex_height)));
  const u32 right =
      static_cast<u32>(std::clamp(source_rc.right, static_cast<int>(left), static_cast<int>(tex_width)));
  const u32 bottom =
      static_cast<u32>(std::clamp(source_rc.bottom, static_cast<int>(top), static_cast<int>(tex_height)));
  const u32 width = right - left;
  const u32 height = bottom - top;
  if (!width || !height)
    return;

  if (m_picture.size() < static_cast<size_t>(width) * height)
    m_picture.resize(static_cast<size_t>(width) * height);

  const u8* src = texture->GetData(0, 0) + (static_cast<size_t>(top) * tex_width + left) * 4;
  u32* dst = m_picture.data();
  for (u32 y = 0; y < height; y++)
  {
    const u8* s = src + static_cast<size_t>(y) * tex_width * 4;
    for (u32 x = 0; x < width; x++, s += 4)
      *dst++ = (static_cast<u32>(s[0]) << 16) | (static_cast<u32>(s[1]) << 8) | s[2];
  }

#ifdef __LIBRETRO__
  Libretro::Video::video_cb(m_picture.data(), width, height, width * 4);
#endif
}

void SWGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                        bool alphaEnable, bool zEnable, u32 color, u32 z)
{
  EfbCopy::ClearEfb();
}

std::unique_ptr<NativeVertexFormat>
SWGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<NativeVertexFormat>(vtx_decl);
}

void SWGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  // BPFunctions calls SetScissorRect with the "best" scissor rect whenever the viewport or scissor
  // changes.  However, the software renderer is actually able to use multiple scissor rects (which
  // is necessary in a few rendering edge cases, such as with Major Minor's Majestic March).
  // Thus, we use this as a signal to update the list of scissor rects, but ignore the parameter.
  Rasterizer::ScissorChanged();
}

SurfaceInfo SWGfx::GetSurfaceInfo() const
{
  return {EFB_WIDTH, EFB_HEIGHT, 1.0f, AbstractTextureFormat::RGBA8};
}

}  // namespace SW
