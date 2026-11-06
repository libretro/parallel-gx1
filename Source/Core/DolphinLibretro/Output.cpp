#include "DolphinLibretro/Output.h"

#include <cstring>

#include "Common/Logging/Log.h"
#include "DolphinLibretro/Common/Globals.h"
#include "VideoCommon/TextureConfig.h"
#ifdef _WIN32
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#endif

namespace Libretro
{
namespace Video
{
static FrameOutput* s_active;

void OutputRepeat()
{
  if (s_active)
    s_active->Repeat();
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
      ERROR_LOG_FMT(VIDEO, "Failed to create output texture {}", i);
      return false;
    }
    m_framebuffers[i] = DX11::DXFramebuffer::Create(m_textures[i].get(), nullptr, {});
    if (!m_framebuffers[i])
    {
      ERROR_LOG_FMT(VIDEO, "Failed to create output framebuffer {}", i);
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
    return m_framebuffers[0].get();
  m_current = m_v2->get_sync_index(m_v2->handle);
  depth = D3D11Suspend();
  m_v2->wait_sync_index(m_v2->handle);
  D3D11Resume(depth);
  return m_framebuffers[m_current].get();
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

    video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);

    static_cast<DX11::Gfx*>(g_gfx.get())->RestoreContext();
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

DX12Output::DX12Output(retro_hw_render_interface_d3d12* iface, u32 width, u32 height)
    : m_iface(iface), m_width(width), m_height(height), m_current(0),
      m_v2(iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2)
{
}

bool DX12Output::Create()
{
  const unsigned mask = m_v2 ? m_iface->get_sync_index_mask(m_iface->handle) : 1u;
  const u32 count = SlotCount(mask);
  u32 i;

  for (i = 0; i < count; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    m_textures[i] = DX12::DXTexture::Create(OutputConfig(m_width, m_height), "LibretroOutput");
    if (!m_textures[i])
    {
      ERROR_LOG_FMT(VIDEO, "Failed to create output texture {}", i);
      return false;
    }
    m_framebuffers[i] = DX12::DXFramebuffer::Create(m_textures[i].get(), nullptr, {});
    if (!m_framebuffers[i])
    {
      ERROR_LOG_FMT(VIDEO, "Failed to create output framebuffer {}", i);
      return false;
    }
  }
  return true;
}

AbstractFramebuffer* DX12Output::Acquire()
{
  if (m_v2)
  {
    /* the frontend is done with what it read from this index's texture
     * last time it was handed over */
    m_current = m_iface->get_sync_index(m_iface->handle);
    m_iface->wait_sync_index(m_iface->handle);
  }
  return m_framebuffers[m_current].get();
}

void DX12Output::Present()
{
  DX12::DXTexture* texture = m_textures[m_current].get();
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

  video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);
}
#endif

#ifdef HAS_VULKAN
VKOutput::VKOutput(retro_hw_render_interface_vulkan* iface, u32 width, u32 height)
    : m_direct(nullptr), m_iface(iface), m_width(width), m_height(height), m_current(0)
{
  memset(m_images, 0, sizeof(m_images));
  memset(&m_direct_image, 0, sizeof(m_direct_image));
  s_active = this;
}

VKOutput::~VKOutput()
{
  if (s_active == this)
    s_active = nullptr;
  DropDirect();
}

void VKOutput::DropDirect()
{
  memset(&m_direct_image, 0, sizeof(m_direct_image));
  m_direct = nullptr;
}

bool VKOutput::Create()
{
  const unsigned mask = m_iface->get_sync_index_mask(m_iface->handle);
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
      ERROR_LOG_FMT(VIDEO, "Failed to create output image {}", i);
      return false;
    }
    m_framebuffers[i] = Vulkan::VKFramebuffer::Create(m_textures[i].get(), nullptr, {});
    if (!m_framebuffers[i])
    {
      ERROR_LOG_FMT(VIDEO, "Failed to create output framebuffer {}", i);
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
  return true;
}

AbstractFramebuffer* VKOutput::Acquire()
{
  Vulkan::VKTexture* texture;

  /* the frontend is done with what it read from this index's image last
   * time it was handed over */
  m_iface->wait_sync_index(m_iface->handle);
  m_current = m_iface->get_sync_index(m_iface->handle);
  texture = m_textures[m_current].get();
  if (!texture)
    return nullptr;

  /* the last picture's contents are not kept */
  texture->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  texture->TransitionToLayout(Vulkan::g_command_buffer_mgr->GetCurrentCommandBuffer(),
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  return m_framebuffers[m_current].get();
}

void VKOutput::Present()
{
  Vulkan::VKTexture* texture = m_textures[m_current].get();

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
  m_direct_image = image;
  m_direct = vk_texture;
  m_iface->set_image(m_iface->handle, &m_direct_image, 0, nullptr, m_iface->queue_index);
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
  return true;
}

void VKOutput::Repeat()
{
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
