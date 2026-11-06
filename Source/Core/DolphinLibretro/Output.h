#pragma once

#include <memory>

#include <rthreads/rthreads.h>

#include "DolphinLibretro/Video.h"
#include "VideoCommon/FrameOutput.h"

namespace Libretro
{
namespace Video
{
/* The output in use, told when the frontend is about to show the last
 * picture again, and asked to hand over what another thread published. */
void OutputRepeat();
void OutputHandOver();

/* A threaded output's pictures between the GPU thread, which publishes
 * them, and the frontend's, which hands them over: the newest published
 * (pending), the one being handed over (taking) and the one shown
 * (handed), each one of the output's images (TARGET, its index) or a
 * texture handed on in place (DIRECT, the index of its description).
 * The frontend may read any of the three. */
enum
{
  PUB_NONE,
  PUB_TARGET,
  PUB_DIRECT
};

enum
{
  /* images of the output's own: the one drawn into, and the two the
   * frontend may read, as one published and not being handed over is
   * drawn into again in place */
  QUEUE_IMAGES = 3,
  /* descriptions of textures handed on in place: at most three read */
  QUEUE_DIRECT = 4
};

struct Published
{
  int kind;
  u32 index;
  const void* texture;
  u32 width;
  u32 height;
  u64 fence_value;
  u64 generation;
};

struct OutputQueue
{
  slock_t* lock;
  Published pending;
  Published taking;
  Published handed;
  u64 generation;
};

void QueueInit(OutputQueue* queue);
void QueueFree(OutputQueue* queue);
/* GPU thread: the image to draw the next picture into */
u32 QueueAcquireTarget(OutputQueue* queue);
/* GPU thread: a description's slot the frontend is not using */
u32 QueueFreeDirect(OutputQueue* queue);
void QueuePublish(OutputQueue* queue, int kind, u32 index, const void* texture, u32 width,
                  u32 height, u64 fence_value);
/* frontend's thread: the picture to hand over, if a new one */
bool QueueTake(OutputQueue* queue, Published* out);
/* frontend's thread: handed over, and the frontend's frame reading it
 * submitted */
void QueueHanded(OutputQueue* queue);
bool QueueHolds(OutputQueue* queue, const void* texture);
/* the textures handed on in place that the frontend may read, at most
 * three */
u32 QueueHeldDirect(OutputQueue* queue, const void** textures);

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
 * where it is; with version 1 one target, which the frontend copies.
 * Threaded, the GPU thread draws and publishes and the frontend's thread
 * hands over (OutputQueue), and a texture handed on in place is kept in
 * the state the frontend reads it in at every execution while the
 * frontend may read it. */
class DX12Output final : public FrameOutput
{
public:
  DX12Output(retro_hw_render_interface_d3d12* iface, u32 width, u32 height, bool threaded);
  ~DX12Output() override;
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height) override;
  void Repeat() override;
  bool IsTextureHeld(const AbstractTexture* texture) override;
  bool HandOver() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  void Hand(DX12::DXTexture* texture, u32 width, u32 height);
  void Publish(int kind, u32 index, DX12::DXTexture* texture, u32 width, u32 height);
  static void BeforeExecute(void* opaque);

  bool m_threaded;
  OutputQueue m_queue;

  std::unique_ptr<DX12::DXTexture> m_textures[OUTPUT_MAX_SLOTS];
  std::unique_ptr<DX12::DXFramebuffer> m_framebuffers[OUTPUT_MAX_SLOTS];
  /* A texture handed on where it is: version 2 reads it until another
   * picture is handed on, so it is kept in the state the frontend reads
   * it in meanwhile. */
  DX12::DXTexture* m_direct;
  retro_hw_render_interface_d3d12* m_iface;
  u32 m_width;
  u32 m_height;
  u32 m_current;
  bool m_v2;
};
#endif

#ifdef HAS_VULKAN
/* Vulkan: one image per sync index, read by the frontend where it is,
 * in shader-read layout, on the queue both submit to.
 *
 * Threaded, the GPU thread draws and publishes and the frontend's thread
 * hands over (OutputQueue), and a texture handed on in place is kept in
 * shader-read layout at every submission while the frontend may read
 * it. */
class VKOutput final : public FrameOutput
{
public:
  VKOutput(retro_hw_render_interface_vulkan* iface, u32 width, u32 height, bool threaded);
  ~VKOutput() override;
  bool Create();

  AbstractFramebuffer* Acquire() override;
  void Present() override;
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height) override;
  void Repeat() override;
  bool IsTextureHeld(const AbstractTexture* texture) override;
  bool HandOver() override;
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }
  AbstractTextureFormat GetFormat() const override { return AbstractTextureFormat::RGBA8; }

private:
  void DropDirect();
  static void BeforeSubmit(void* opaque, VkCommandBuffer command_buffer);

  bool m_threaded;
  OutputQueue m_queue;
  retro_vulkan_image m_direct_images[QUEUE_DIRECT];

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
