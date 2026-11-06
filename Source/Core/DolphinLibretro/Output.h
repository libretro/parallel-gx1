#pragma once

#include <memory>

#include "DolphinLibretro/Video.h"
#include "VideoCommon/FrameOutput.h"

namespace Libretro
{
namespace Video
{
/* The output in use, told when the frontend is about to show the last
 * picture again. */
void OutputRepeat();

/* What each sync index of the frontend can name: the frontend reads the
 * picture where it is drawn, so there is one target per index the
 * frontend can be reading from while the next picture is drawn. */
enum
{
  OUTPUT_MAX_SLOTS = 32
};

#ifdef _WIN32
/* Direct3D 11: with version 2 of the interface one target per sync
 * index, named with set_texture and read where it is; with version 1
 * one target, which the frontend reads from pixel shader slot 0 while
 * video_refresh runs. */
class DX11Output final : public FrameOutput
{
public:
  DX11Output(retro_hw_render_interface_d3d11* v2, u32 width, u32 height);
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height) override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  std::unique_ptr<DX11::DXTexture> m_textures[OUTPUT_MAX_SLOTS];
  std::unique_ptr<DX11::DXFramebuffer> m_framebuffers[OUTPUT_MAX_SLOTS];
  retro_hw_render_interface_d3d11* m_v2;   /* NULL on version 1 */
  u32 m_width;
  u32 m_height;
  u32 m_current;
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
  ~VKOutput() override;
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height) override;
  void Repeat() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  void DropDirect();

  retro_vulkan_image m_images[OUTPUT_MAX_SLOTS];
  std::unique_ptr<Vulkan::VKTexture> m_textures[OUTPUT_MAX_SLOTS];
  std::unique_ptr<Vulkan::VKFramebuffer> m_framebuffers[OUTPUT_MAX_SLOTS];
  /* A texture handed on where it is, through its 2D view: the frontend
   * reads it until another picture is handed on, so it is kept in
   * shader-read layout meanwhile. */
  retro_vulkan_image m_direct_image;
  Vulkan::VKTexture* m_direct;
  retro_hw_render_interface_vulkan* m_iface;
  u32 m_width;
  u32 m_height;
  u32 m_current;
};
#endif
}  // namespace Video
}  // namespace Libretro
