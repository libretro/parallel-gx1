#include "DolphinLibretro/Output.h"

#include <cstring>

#include "Common/Logging/Log.h"
#include "DolphinLibretro/Common/Globals.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoBackendKind.h"
#ifdef _WIN32
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#endif

/* FrameOutput calls the output of the backend in use. */
#ifdef _WIN32
#define OUTPUT_D3D11_CASE(call)                                                                    \
  case VIDEO_BACKEND_D3D11:                                                                        \
    return static_cast<Libretro::Video::DX11Output*>(this)->call;
#define OUTPUT_D3D12_CASE(call)                                                                    \
  case VIDEO_BACKEND_D3D12:                                                                        \
    return static_cast<Libretro::Video::DX12Output*>(this)->call;
#else
#define OUTPUT_D3D11_CASE(call)
#define OUTPUT_D3D12_CASE(call)
#endif
#ifdef HAS_VULKAN
#define OUTPUT_VULKAN_CASE(call)                                                                   \
  case VIDEO_BACKEND_VULKAN:                                                                       \
    return static_cast<Libretro::Video::VKOutput*>(this)->call;
#else
#define OUTPUT_VULKAN_CASE(call)
#endif

void FrameOutputDelete(FrameOutput* object)
{
  if (!object)
    return;
  switch (g_video_backend_kind)
  {
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    delete static_cast<Libretro::Video::DX11Output*>(object);
    return;
  case VIDEO_BACKEND_D3D12:
    delete static_cast<Libretro::Video::DX12Output*>(object);
    return;
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    delete static_cast<Libretro::Video::VKOutput*>(object);
    return;
#endif
  default:
    return;
  }
}

AbstractFramebuffer* FrameOutput::Acquire()
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D11_CASE(Acquire())
    OUTPUT_D3D12_CASE(Acquire())
    OUTPUT_VULKAN_CASE(Acquire())
  default:
    return nullptr;
  }
}

void FrameOutput::Present()
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D11_CASE(Present())
    OUTPUT_D3D12_CASE(Present())
    OUTPUT_VULKAN_CASE(Present())
  default:
    return;
  }
}

bool FrameOutput::PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D11_CASE(PresentTexture(texture, width, height))
    OUTPUT_D3D12_CASE(PresentTexture(texture, width, height))
    OUTPUT_VULKAN_CASE(PresentTexture(texture, width, height))
  default:
    return false;
  }
}

void FrameOutput::KeepReadable()
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D12_CASE(KeepReadable())
    OUTPUT_VULKAN_CASE(KeepReadable())
  default:
    return;
  }
}

bool FrameOutput::IsTextureHeld(const AbstractTexture* texture)
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D12_CASE(IsTextureHeld(texture))
    OUTPUT_VULKAN_CASE(IsTextureHeld(texture))
  default:
    return false;
  }
}

bool FrameOutput::HandOver()
{
  switch (g_video_backend_kind)
  {
    OUTPUT_D3D12_CASE(HandOver())
    OUTPUT_VULKAN_CASE(HandOver())
  default:
    return false;
  }
}

u32 FrameOutput::GetWidth() const
{
  switch (g_video_backend_kind)
  {
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const Libretro::Video::DX11Output*>(this)->GetWidth();
  case VIDEO_BACKEND_D3D12:
    return static_cast<const Libretro::Video::DX12Output*>(this)->GetWidth();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Libretro::Video::VKOutput*>(this)->GetWidth();
#endif
  default:
    return 0;
  }
}

u32 FrameOutput::GetHeight() const
{
  switch (g_video_backend_kind)
  {
#ifdef _WIN32
  case VIDEO_BACKEND_D3D11:
    return static_cast<const Libretro::Video::DX11Output*>(this)->GetHeight();
  case VIDEO_BACKEND_D3D12:
    return static_cast<const Libretro::Video::DX12Output*>(this)->GetHeight();
#endif
#ifdef HAS_VULKAN
  case VIDEO_BACKEND_VULKAN:
    return static_cast<const Libretro::Video::VKOutput*>(this)->GetHeight();
#endif
  default:
    return 0;
  }
}

#undef OUTPUT_D3D11_CASE
#undef OUTPUT_D3D12_CASE
#undef OUTPUT_VULKAN_CASE

namespace Libretro
{
namespace Video
{
static FrameOutput* s_active;

void OutputKeepReadable()
{
  if (s_active)
    s_active->KeepReadable();
}

void OutputHandOver()
{
  if (s_active)
    s_active->HandOver();
}

enum
{
  QUEUE_TAKEN = 0x40000000,
  QUEUE_GENERATION_MASK = 0x3fffffff
};

static int QueueTag(int kind, u32 index)
{
  return ((kind << 8) | int(index)) + 1;
}

void QueueInit(OutputQueue* queue)
{
  u32 i;

  for (i = 0; i < 4; i++)
  {
    retro_atomic_int_init(&queue->slots[i].kind_index, PUB_NONE << 8);
    retro_atomic_int_init(&queue->slots[i].size, 0);
    retro_atomic_store_relaxed_ptr(&queue->slots[i].texture, nullptr);
    retro_atomic_64_init(&queue->slots[i].fence_value, 0);
  }
  retro_atomic_int_init(&queue->pending, 0);
  retro_atomic_int_init(&queue->taking, 0);
  retro_atomic_int_init(&queue->handed, 0);
  queue->generation = 0;
  for (i = 0; i < QUEUE_DIRECT; i++)
    queue->direct_textures[i] = nullptr;
}

static PublishedSlot* QueueSlot(OutputQueue* queue, int pending)
{
  return &queue->slots[pending & 3];
}

/* GPU thread: whether an image or description is published, being handed
 * over or handed; the consumer only ever newly holds the pending one */
static bool QueueInUse(OutputQueue* queue, int kind, u32 index)
{
  const int tag = QueueTag(kind, index);
  const int pending = retro_atomic_load_acquire_int(&queue->pending);

  if (pending &&
      retro_atomic_load_relaxed_int(&QueueSlot(queue, pending)->kind_index) == tag - 1)
  {
    return true;
  }
  return retro_atomic_load_acquire_int(&queue->taking) == tag ||
         retro_atomic_load_acquire_int(&queue->handed) == tag;
}

u32 QueueAcquireTarget(OutputQueue* queue)
{
  const int pending = retro_atomic_load_acquire_int(&queue->pending);
  u32 i;

  /* the picture published and not taken is not wanted any more: drawn
   * into again, in place, unless the consumer claims it first */
  if (pending && !(pending & QUEUE_TAKEN))
  {
    const int kind_index = retro_atomic_load_relaxed_int(&QueueSlot(queue, pending)->kind_index);
    if ((kind_index >> 8) == PUB_TARGET && retro_atomic_cas_int(&queue->pending, pending, 0))
      return u32(kind_index & 0xff);
  }

  for (i = 0; i < QUEUE_IMAGES && QueueInUse(queue, PUB_TARGET, i); i++)
  {
  }
  return i;
}

u32 QueueFreeDirect(OutputQueue* queue)
{
  u32 i;

  for (i = 0; i < QUEUE_DIRECT && QueueInUse(queue, PUB_DIRECT, i); i++)
  {
  }
  return i;
}

void QueuePublish(OutputQueue* queue, int kind, u32 index, const void* texture, u32 width,
                  u32 height, u64 fence_value)
{
  PublishedSlot* slot;
  int generation = (queue->generation + 1) & QUEUE_GENERATION_MASK;

  if (!generation)
    generation = 1;
  queue->generation = generation;

  slot = QueueSlot(queue, generation);
  retro_atomic_store_relaxed_int(&slot->kind_index, QueueTag(kind, index) - 1);
  retro_atomic_store_relaxed_int(&slot->size, int((width << 16) | (height & 0xffff)));
  retro_atomic_store_relaxed_ptr(&slot->texture, const_cast<void*>(texture));
  retro_atomic_store_relaxed_64(&slot->fence_value, s64(fence_value));
  if (kind == PUB_DIRECT && index < QUEUE_DIRECT)
    queue->direct_textures[index] = texture;

  /* the slot's fields before the generation that names it */
  retro_atomic_exchange_int(&queue->pending, generation);
}

bool QueueTake(OutputQueue* queue, Published* out)
{
  for (;;)
  {
    const int pending = retro_atomic_load_acquire_int(&queue->pending);
    PublishedSlot* slot;

    if (!pending || (pending & QUEUE_TAKEN))
      return false;

    slot = QueueSlot(queue, pending);
    {
      const int kind_index = retro_atomic_load_relaxed_int(&slot->kind_index);
      const u32 size = u32(retro_atomic_load_relaxed_int(&slot->size));
      out->kind = kind_index >> 8;
      out->index = u32(kind_index & 0xff);
      out->width = size >> 16;
      out->height = size & 0xffff;
    }
    out->texture = retro_atomic_load_relaxed_ptr(&slot->texture);
    out->fence_value = u64(retro_atomic_load_relaxed_64(&slot->fence_value));

    /* held before claimed, so the producer never sees it free in between;
     * the copy is whole if the claim succeeds, as the slot is rewritten
     * only after four more publishes */
    retro_atomic_store_release_int(&queue->taking, QueueTag(out->kind, out->index));
    if (retro_atomic_cas_int(&queue->pending, pending, pending | QUEUE_TAKEN))
      return true;
    retro_atomic_store_release_int(&queue->taking, 0);
  }
}

void QueueHanded(OutputQueue* queue)
{
  retro_atomic_store_release_int(&queue->handed, retro_atomic_load_relaxed_int(&queue->taking));
  retro_atomic_store_release_int(&queue->taking, 0);
}

/* GPU thread: the texture of a held description */
static const void* QueueHeldTexture(OutputQueue* queue, int tag)
{
  const int kind = (tag - 1) >> 8;
  const u32 index = u32((tag - 1) & 0xff);

  if (!tag || kind != PUB_DIRECT || index >= QUEUE_DIRECT)
    return nullptr;
  return queue->direct_textures[index];
}

u32 QueueHeldDirect(OutputQueue* queue, const void** textures)
{
  const int pending = retro_atomic_load_acquire_int(&queue->pending);
  const void* texture;
  u32 n = 0;

  if (pending)
  {
    PublishedSlot* slot = QueueSlot(queue, pending);
    if ((retro_atomic_load_relaxed_int(&slot->kind_index) >> 8) == PUB_DIRECT)
      textures[n++] = retro_atomic_load_relaxed_ptr(&slot->texture);
  }
  if ((texture = QueueHeldTexture(queue, retro_atomic_load_acquire_int(&queue->taking))))
    textures[n++] = texture;
  if ((texture = QueueHeldTexture(queue, retro_atomic_load_acquire_int(&queue->handed))))
    textures[n++] = texture;
  return n;
}

bool QueueHolds(OutputQueue* queue, const void* texture)
{
  const void* held[3];
  u32 n = QueueHeldDirect(queue, held);
  u32 i;

  for (i = 0; i < n; i++)
  {
    if (held[i] == texture)
      return true;
  }
  return false;
}

#if defined(_WIN32) || defined(HAS_VULKAN)
/* The sync indices a mask names: one past the highest set bit. */
static u32 SlotCount(unsigned mask)
{
  u32 n = 0;
  while (mask && n < OUTPUT_MAX_SLOTS)
  {
    mask >>= 1;
    n++;
  }
  return n;
}

static TextureConfig OutputConfig(u32 width, u32 height)
{
  return TextureConfig(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8,
                       AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);
}
#endif

#ifdef _WIN32
DX11Output::DX11Output(retro_hw_render_interface_d3d11* v2, u32 width, u32 height)
    : m_v2(v2), m_width(width), m_height(height), m_current(0)
{
}

DX11Output::~DX11Output()
{
  for (u32 i = 0; i < OUTPUT_MAX_SLOTS; i++)
  {
    delete m_framebuffers[i];
    delete m_textures[i];
  }
}

bool DX11Output::Create()
{
  const unsigned mask = m_v2 ? m_v2->get_sync_index_mask(m_v2->handle) : 1u;
  const u32 count = SlotCount(mask);
  u32 i;

  for (i = 0; i < count; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    m_textures[i] = DX11::DXTexture::Create(OutputConfig(m_width, m_height), "LibretroOutput");
    if (!m_textures[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output texture %u", i);
      return false;
    }
    m_framebuffers[i] = DX11::DXFramebuffer::Create(m_textures[i], nullptr);
    if (!m_framebuffers[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output framebuffer %u", i);
      return false;
    }
  }
  return true;
}

AbstractFramebuffer* DX11Output::Acquire()
{
  unsigned depth;
  /* version 1: the frontend read the last picture inside video_refresh;
   * version 2: it is done with this index's texture once the wait
   * returns, which needs the context lock released */
  if (!m_v2)
    return m_framebuffers[0];
  m_current = m_v2->get_sync_index(m_v2->handle);
  depth = D3D11Suspend();
  m_v2->wait_sync_index(m_v2->handle);
  D3D11Resume(depth);
  return m_framebuffers[m_current];
}

void DX11Output::Present()
{
  unsigned depth;

  if (m_v2)
  {
    m_v2->set_texture(m_v2->handle, m_textures[m_current]->GetD3DTexture());
    depth = D3D11Suspend();
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);
    /* rebinds everything if the frontend drew meanwhile */
    D3D11Resume(depth);
    return;
  }

  {
    ID3D11ShaderResourceView* srv = m_textures[0]->GetD3DSRV();
    ID3D11RenderTargetView* null_view = nullptr;
    /* none of the core's constant buffers left for the frontend's
     * shader passes, which bind theirs only for the stages that use
     * them */
    ID3D11Buffer* null_buffers[3] = {nullptr, nullptr, nullptr};

    DX11::D3D::context->OMSetRenderTargets(1, &null_view, nullptr);
    DX11::D3D::context->PSSetShaderResources(0, 1, &srv);
    DX11::D3D::context->VSSetConstantBuffers(0, 3, null_buffers);
    DX11::D3D::context->PSSetConstantBuffers(0, 3, null_buffers);
    DX11::D3D::context->GSSetConstantBuffers(0, 1, null_buffers);

    RefreshNow(m_width, m_height);

    static_cast<DX11::Gfx*>(g_gfx)->RestoreContext();
  }
}

bool DX11Output::PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
{
  unsigned depth;

  /* version 2 only: version 1 reads pixel shader slot 0, which needs a
   * view of one 2D texture. The immediate context orders the frontend's
   * reads before anything drawn into the texture after; no sync index
   * is waited on, as none of the output's targets is drawn into. */
  if (!m_v2)
    return false;
  m_v2->set_texture(m_v2->handle,
                    static_cast<const DX11::DXTexture*>(texture)->GetD3DTexture());
  depth = D3D11Suspend();
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
  D3D11Resume(depth);
  return true;
}

DX12Output::DX12Output(retro_hw_render_interface_d3d12* iface, u32 width, u32 height,
                       bool threaded)
    : m_threaded(threaded), m_direct(nullptr), m_iface(iface), m_width(width),
      m_height(height), m_current(0),
      m_v2(iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2)
{
  QueueInit(&m_queue);
  s_active = this;
}

DX12Output::~DX12Output()
{
  if (s_active == this)
    s_active = nullptr;
  if (m_threaded && DX12::g_dx_context)
    DX12::g_dx_context->SetBeforeExecute(nullptr, nullptr);
  for (u32 i = 0; i < OUTPUT_MAX_SLOTS; i++)
  {
    delete m_framebuffers[i];
    delete m_textures[i];
  }
}

bool DX12Output::Create()
{
  /* threaded, the targets are this output's own, not the sync indices' */
  const unsigned mask = m_threaded ? (1u << QUEUE_IMAGES) - 1 :
                        m_v2       ? m_iface->get_sync_index_mask(m_iface->handle) :
                                     1u;
  const u32 count = SlotCount(mask);
  u32 i;

  for (i = 0; i < count; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    m_textures[i] = DX12::DXTexture::Create(OutputConfig(m_width, m_height), "LibretroOutput");
    if (!m_textures[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output texture %u", i);
      return false;
    }
    m_framebuffers[i] = DX12::DXFramebuffer::Create(m_textures[i], nullptr);
    if (!m_framebuffers[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output framebuffer %u", i);
      return false;
    }
  }
  if (m_threaded)
    DX12::g_dx_context->SetBeforeExecute(BeforeExecute, this);
  return true;
}

AbstractFramebuffer* DX12Output::Acquire()
{
  if (m_threaded)
  {
    /* a target the frontend does not read */
    m_current = QueueAcquireTarget(&m_queue);
    return m_framebuffers[m_current];
  }
  if (m_v2)
  {
    /* the frontend is done with what it read from this index's texture
     * last time it was handed over */
    m_current = m_iface->get_sync_index(m_iface->handle);
    m_iface->wait_sync_index(m_iface->handle);
  }
  return m_framebuffers[m_current];
}

void DX12Output::Present()
{
  m_direct = nullptr;
  if (m_threaded)
  {
    Publish(PUB_TARGET, m_current, m_textures[m_current], m_width, m_height);
    return;
  }
  Hand(m_textures[m_current], m_width, m_height);
}

bool DX12Output::PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
{
  /* version 1 copies it inside video_refresh, version 2 reads it where it
   * is; neither waits on a sync index, as none of the output's textures
   * is drawn into */
  m_direct = const_cast<DX12::DXTexture*>(static_cast<const DX12::DXTexture*>(texture));
  if (m_threaded)
  {
    Publish(PUB_DIRECT, QueueFreeDirect(&m_queue), m_direct, width, height);
    return true;
  }
  Hand(m_direct, width, height);
  return true;
}

void DX12Output::Publish(int kind, u32 index, DX12::DXTexture* texture, u32 width, u32 height)
{
  u64 fence_value;

  texture->TransitionToState(m_iface->required_state);
  fence_value = DX12::g_dx_context->GetCurrentFenceValue();
  DX12::Gfx::GetInstance()->ExecuteCommandList(false);
  QueuePublish(&m_queue, kind, index, texture, width, height, fence_value);
}

void DX12Output::BeforeExecute(void* opaque)
{
  DX12Output* self = static_cast<DX12Output*>(opaque);
  const void* held[3];
  u32 n;
  u32 i;

  /* the frontend executes whenever its frame is due: whatever it may read
   * is left in the state it reads it in by every execution of the core's,
   * should the core have used it in another since, as a copy's source
   * say */
  n = QueueHeldDirect(&self->m_queue, held);
  for (i = 0; i < n; i++)
  {
    const DX12::DXTexture* texture = static_cast<const DX12::DXTexture*>(held[i]);
    if (texture->GetState() != self->m_iface->required_state)
      texture->TransitionToState(self->m_iface->required_state);
  }
}

bool DX12Output::IsTextureHeld(const AbstractTexture* texture)
{
  if (!m_threaded)
    return texture == m_direct;
  return QueueHolds(&m_queue, texture);
}

bool DX12Output::HandOver()
{
  Published picture;
  ID3D12Resource* resource;

  if (!m_threaded || !QueueTake(&m_queue, &picture))
    return false;
  resource = static_cast<const DX12::DXTexture*>(picture.texture)->GetResource();
  if (m_v2)
  {
    m_iface->set_texture_fenced(m_iface->handle, resource, DXGI_FORMAT_R8G8B8A8_UNORM,
                                DX12::g_dx_context->GetFence(), picture.fence_value);
  }
  else
  {
    m_iface->set_texture(m_iface->handle, resource, DXGI_FORMAT_R8G8B8A8_UNORM);
  }
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, picture.width, picture.height, 0);
  QueueHanded(&m_queue);
  return true;
}

void DX12Output::KeepReadable()
{
  /* threaded, every execution already leaves it readable */
  if (m_threaded)
    return;
  /* version 2 reads the texture again: back to the state it reads it in
   * if the core used it in another one meanwhile, a copy's source say,
   * ahead of the frontend's next frame on the queue */
  if (!m_v2 || !m_direct || m_direct->GetState() == m_iface->required_state)
    return;
  m_direct->TransitionToState(m_iface->required_state);
  DX12::Gfx::GetInstance()->ExecuteCommandList(false);
}

void DX12Output::Hand(DX12::DXTexture* texture, u32 width, u32 height)
{
  u64 fence_value;

  texture->TransitionToState(m_iface->required_state);
  fence_value = DX12::g_dx_context->GetCurrentFenceValue();
  DX12::Gfx::GetInstance()->ExecuteCommandList(false);

  if (m_v2)
  {
    m_iface->set_texture_fenced(m_iface->handle, texture->GetResource(),
                                DXGI_FORMAT_R8G8B8A8_UNORM, DX12::g_dx_context->GetFence(),
                                fence_value);
  }
  else
  {
    m_iface->set_texture(m_iface->handle, texture->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM);
  }

  video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
}
#endif

#ifdef HAS_VULKAN
VKOutput::VKOutput(retro_hw_render_interface_vulkan* iface, u32 width, u32 height,
                   bool threaded)
    : m_threaded(threaded), m_direct(nullptr), m_iface(iface), m_width(width),
      m_height(height), m_current(0)
{
  QueueInit(&m_queue);
  memset(m_direct_images, 0, sizeof(m_direct_images));
  memset(m_images, 0, sizeof(m_images));
  memset(&m_direct_image, 0, sizeof(m_direct_image));
  s_active = this;
}

VKOutput::~VKOutput()
{
  if (s_active == this)
    s_active = nullptr;
  if (m_threaded && Vulkan::g_command_buffer_mgr)
    Vulkan::g_command_buffer_mgr->SetBeforeSubmit(nullptr, nullptr);
  DropDirect();
  for (u32 i = 0; i < OUTPUT_MAX_SLOTS; i++)
  {
    delete m_framebuffers[i];
    delete m_textures[i];
  }
}

void VKOutput::DropDirect()
{
  memset(&m_direct_image, 0, sizeof(m_direct_image));
  m_direct = nullptr;
}

bool VKOutput::Create()
{
  /* threaded, the images are this output's own, not the sync indices' */
  const unsigned mask = m_threaded ? (1u << QUEUE_IMAGES) - 1 :
                                     m_iface->get_sync_index_mask(m_iface->handle);
  const u32 count = SlotCount(mask);
  u32 i;

  for (i = 0; i < count; i++)
  {
    retro_vulkan_image* image = &m_images[i];
    if (!(mask & (1u << i)))
      continue;
    m_textures[i] = Vulkan::VKTexture::Create(OutputConfig(m_width, m_height), "LibretroOutput");
    if (!m_textures[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output image %u", i);
      return false;
    }
    m_framebuffers[i] = Vulkan::VKFramebuffer::Create(m_textures[i], nullptr);
    if (!m_framebuffers[i])
    {
      ERROR_LOG(VIDEO, "Failed to create output framebuffer %u", i);
      return false;
    }

    image->image_view = m_textures[i]->GetView();
    image->image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image->create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image->create_info.image = m_textures[i]->GetImage();
    image->create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image->create_info.format = m_textures[i]->GetVkFormat();
    image->create_info.components.r = VK_COMPONENT_SWIZZLE_R;
    image->create_info.components.g = VK_COMPONENT_SWIZZLE_G;
    image->create_info.components.b = VK_COMPONENT_SWIZZLE_B;
    image->create_info.components.a = VK_COMPONENT_SWIZZLE_A;
    image->create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image->create_info.subresourceRange.baseMipLevel = 0;
    image->create_info.subresourceRange.levelCount = 1;
    image->create_info.subresourceRange.baseArrayLayer = 0;
    image->create_info.subresourceRange.layerCount = 1;
  }
  if (m_threaded)
    Vulkan::g_command_buffer_mgr->SetBeforeSubmit(BeforeSubmit, this);
  return true;
}

void VKOutput::BeforeSubmit(void* opaque, VkCommandBuffer command_buffer)
{
  VKOutput* self = static_cast<VKOutput*>(opaque);
  const void* held[3];
  u32 n;
  u32 i;

  /* the frontend submits whenever its frame is due: whatever it may read
   * is left readable by every submission of the core's, should the core
   * have read it in another layout since, as a copy's source say */
  n = QueueHeldDirect(&self->m_queue, held);
  for (i = 0; i < n; i++)
  {
    const Vulkan::VKTexture* texture = static_cast<const Vulkan::VKTexture*>(held[i]);
    if (texture->GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      texture->TransitionToLayout(command_buffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

bool VKOutput::IsTextureHeld(const AbstractTexture* texture)
{
  if (!m_threaded)
    return texture == m_direct;
  return QueueHolds(&m_queue, texture);
}

bool VKOutput::HandOver()
{
  Published picture;
  retro_vulkan_image* image;

  if (!m_threaded || !QueueTake(&m_queue, &picture))
    return false;
  image = picture.kind == PUB_TARGET ? &m_images[picture.index] :
                                       &m_direct_images[picture.index];
  m_iface->set_image(m_iface->handle, image, 0, nullptr, m_iface->queue_index);
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, picture.width, picture.height, 0);
  /* the frontend's frame that reads it is submitted: the one shown before
   * is free from here on */
  QueueHanded(&m_queue);
  return true;
}

AbstractFramebuffer* VKOutput::Acquire()
{
  Vulkan::VKTexture* texture;

  if (m_threaded)
  {
    /* an image the frontend does not read */
    m_current = QueueAcquireTarget(&m_queue);
  }
  else
  {
    /* the frontend is done with what it read from this index's image
     * last time it was handed over */
    m_iface->wait_sync_index(m_iface->handle);
    m_current = m_iface->get_sync_index(m_iface->handle);
  }
  texture = m_textures[m_current];
  if (!texture)
    return nullptr;

  /* the last picture's contents are not kept */
  texture->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  texture->TransitionToLayout(Vulkan::g_command_buffer_mgr->GetCurrentCommandBuffer(),
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  return m_framebuffers[m_current];
}

void VKOutput::Present()
{
  Vulkan::VKTexture* texture = m_textures[m_current];

  if (!texture)
  {
    Vulkan::g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);
    return;
  }

  /* the layout the frontend reads it in; submitted here, on this thread,
   * before the frontend is told, so its reads follow on the queue */
  texture->TransitionToLayout(Vulkan::g_command_buffer_mgr->GetCurrentCommandBuffer(),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Vulkan::g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);

  DropDirect();
  if (m_threaded)
  {
    QueuePublish(&m_queue, PUB_TARGET, m_current, texture, m_width, m_height, 0);
    return;
  }
  m_iface->set_image(m_iface->handle, &m_images[m_current], 0, nullptr, m_iface->queue_index);
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);
}

bool VKOutput::PresentTexture(const AbstractTexture* texture, u32 width, u32 height)
{
  Vulkan::VKTexture* vk_texture =
      const_cast<Vulkan::VKTexture*>(static_cast<const Vulkan::VKTexture*>(texture));
  retro_vulkan_image image;

  /* the texture's own 2D view of its one layer: the frontend's shaders
   * read a 2D image */
  if (vk_texture->GetView2D() == VK_NULL_HANDLE)
    return false;

  memset(&image, 0, sizeof(image));
  image.image_view = vk_texture->GetView2D();
  image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image.create_info.image = vk_texture->GetImage();
  image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image.create_info.format = vk_texture->GetVkFormat();
  image.create_info.components.r = VK_COMPONENT_SWIZZLE_R;
  image.create_info.components.g = VK_COMPONENT_SWIZZLE_G;
  image.create_info.components.b = VK_COMPONENT_SWIZZLE_B;
  image.create_info.components.a = VK_COMPONENT_SWIZZLE_A;
  image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image.create_info.subresourceRange.baseMipLevel = 0;
  image.create_info.subresourceRange.levelCount = 1;
  image.create_info.subresourceRange.baseArrayLayer = 0;
  image.create_info.subresourceRange.layerCount = 1;
  vk_texture->TransitionToLayout(Vulkan::g_command_buffer_mgr->GetCurrentCommandBuffer(),
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Vulkan::g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);

  DropDirect();
  if (m_threaded)
  {
    /* a slot for its description the frontend is not using */
    const u32 slot = QueueFreeDirect(&m_queue);
    m_direct_images[slot] = image;
    QueuePublish(&m_queue, PUB_DIRECT, slot, vk_texture, width, height, 0);
    return true;
  }
  m_direct_image = image;
  m_direct = vk_texture;
  m_iface->set_image(m_iface->handle, &m_direct_image, 0, nullptr, m_iface->queue_index);
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
  return true;
}

void VKOutput::KeepReadable()
{
  /* threaded, every submission already leaves it readable */
  if (m_threaded)
    return;
  /* the core may have read the texture since in a layout of its own, a
   * copy's source say: back to the one the frontend reads it in, ahead
   * of the frontend's next frame on the queue */
  if (!m_direct || m_direct->GetLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    return;
  Vulkan::StateTracker::GetInstance()->EndRenderPass();
  Vulkan::g_command_buffer_mgr->WaitForWorkerThreadIdle();
  m_direct->TransitionToLayout(Vulkan::g_command_buffer_mgr->GetCurrentCommandBuffer(),
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Vulkan::g_command_buffer_mgr->SubmitCommandBuffer(false, false, false);
  Vulkan::StateTracker::GetInstance()->InvalidateCachedState();
}
#endif
}  // namespace Video
}  // namespace Libretro
