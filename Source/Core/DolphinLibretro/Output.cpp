#include "DolphinLibretro/Output.h"

#include <cstring>

#include "Common/Logging/Log.h"
#include "DolphinLibretro/Common/Globals.h"
#include "VideoCommon/TextureConfig.h"
#ifdef _WIN32
#include "VideoBackends/D3D12/D3D12Gfx.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#endif

namespace Libretro
{
namespace Video
{
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
DX11Output::DX11Output(u32 width, u32 height) : m_width(width), m_height(height)
{
}

bool DX11Output::Create()
{
  m_texture = DX11::DXTexture::Create(OutputConfig(m_width, m_height), "LibretroOutput");
  if (!m_texture)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create the output texture");
    return false;
  }
  m_framebuffer = DX11::DXFramebuffer::Create(m_texture.get(), nullptr, {});
  if (!m_framebuffer)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create the output framebuffer");
    return false;
  }
  return true;
}

AbstractFramebuffer* DX11Output::Acquire()
{
  /* the frontend read the last picture inside video_refresh */
  return m_framebuffer.get();
}

void DX11Output::Present()
{
  ID3D11ShaderResourceView* srv = m_texture->GetD3DSRV();
  ID3D11RenderTargetView* null_view = nullptr;
  /* none of the core's constant buffers left for the frontend's shader
   * passes, which bind theirs only for the stages that use them */
  ID3D11Buffer* null_buffers[3] = {nullptr, nullptr, nullptr};

  DX11::D3D::context->OMSetRenderTargets(1, &null_view, nullptr);
  DX11::D3D::context->PSSetShaderResources(0, 1, &srv);
  DX11::D3D::context->VSSetConstantBuffers(0, 3, null_buffers);
  DX11::D3D::context->PSSetConstantBuffers(0, 3, null_buffers);
  DX11::D3D::context->GSSetConstantBuffers(0, 1, null_buffers);

  video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);

  DX11::D3D::stateman->Restore();
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
    : m_iface(iface), m_width(width), m_height(height), m_current(0)
{
  memset(m_images, 0, sizeof(m_images));
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

  m_iface->set_image(m_iface->handle, &m_images[m_current], 0, nullptr, m_iface->queue_index);
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_width, m_height, 0);
}
#endif
}  // namespace Video
}  // namespace Libretro
