#pragma once

#include <memory>

#include "DolphinLibretro/Video.h"
#include "VideoCommon/FrameOutput.h"

namespace Libretro
{
namespace Video
{
/* What each sync index of the frontend can name: the frontend reads the
 * picture where it is drawn, so there is one target per index the
 * frontend can be reading from while the next picture is drawn. */
enum
{
  OUTPUT_MAX_SLOTS = 32
};

#ifdef _WIN32
/* Direct3D 11: one target, which the frontend reads from pixel shader
 * slot 0 while video_refresh runs. */
class DX11Output final : public FrameOutput
{
public:
  DX11Output(u32 width, u32 height);
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  std::unique_ptr<DX11::DXTexture> m_texture;
  std::unique_ptr<DX11::DXFramebuffer> m_framebuffer;
  u32 m_width;
  u32 m_height;
};

/* Direct3D 12: with version 2 of the interface one target per sync
 * index, handed over with the fence its picture completes on and read
 * where it is; with version 1 one target, which the frontend copies. */
class DX12Output final : public FrameOutput
{
public:
  DX12Output(retro_hw_render_interface_d3d12* iface, u32 width, u32 height);
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  std::unique_ptr<DX12::DXTexture> m_textures[OUTPUT_MAX_SLOTS];
  std::unique_ptr<DX12::DXFramebuffer> m_framebuffers[OUTPUT_MAX_SLOTS];
  retro_hw_render_interface_d3d12* m_iface;
  u32 m_width;
  u32 m_height;
  u32 m_current;
  bool m_v2;
};
#endif

#ifdef HAS_VULKAN
/* Vulkan: one image per sync index, read by the frontend where it is,
 * in shader-read layout, on the queue both submit to. */
class VKOutput final : public FrameOutput
{
public:
  VKOutput(retro_hw_render_interface_vulkan* iface, u32 width, u32 height);
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  retro_vulkan_image m_images[OUTPUT_MAX_SLOTS];
  std::unique_ptr<Vulkan::VKTexture> m_textures[OUTPUT_MAX_SLOTS];
  std::unique_ptr<Vulkan::VKFramebuffer> m_framebuffers[OUTPUT_MAX_SLOTS];
  retro_hw_render_interface_vulkan* m_iface;
  u32 m_width;
  u32 m_height;
  u32 m_current;
};
#endif
}  // namespace Video
}  // namespace Libretro
