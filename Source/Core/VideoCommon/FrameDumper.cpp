// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FrameDumper.h"

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/Thread.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Present.h"
#include "Core/Core.h"

static bool DumpFrameToPNG(const FrameData& frame, const std::string& file_name)
{
  return Common::ConvertRGBAToRGBAndSavePNG(file_name, frame.data, frame.width, frame.height,
                                            frame.stride,
                                            Config::Get(Config::GFX_PNG_COMPRESSION_LEVEL));
}

FrameDumper::FrameDumper()
{
  m_frame_end_handle =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { FlushFrameDump(); });
}

FrameDumper::~FrameDumper()
{
  ShutdownFrameDumping();
  delete static_cast<std::string*>(retro_atomic_exchange_ptr(&m_screenshot_request, nullptr));
}

void FrameDumper::DumpCurrentFrame(const AbstractTexture* src_texture,
                                   const MathUtil::Rectangle<int>& src_rect,
                                   const MathUtil::Rectangle<int>& target_rect)
{
  int source_width = src_rect.GetWidth();
  int source_height = src_rect.GetHeight();
  int target_width = target_rect.GetWidth();
  int target_height = target_rect.GetHeight();

  // We only need to render a copy if we need to stretch/scale the XFB copy.
  MathUtil::Rectangle<int> copy_rect = src_rect;
  if (source_width != target_width || source_height != target_height)
  {
    if (!CheckFrameDumpRenderTexture(target_width, target_height))
      return;

    g_gfx->ScaleTexture(m_frame_dump_render_framebuffer.get(),
                        m_frame_dump_render_framebuffer->GetRect(), src_texture, src_rect);
    src_texture = m_frame_dump_render_texture.get();
    copy_rect = src_texture->GetRect();
  }

  if (!CheckFrameDumpReadbackTexture(target_width, target_height))
    return;

  m_frame_dump_readback_texture->CopyFromTexture(src_texture, copy_rect, 0, 0,
                                                 m_frame_dump_readback_texture->GetRect());
  m_frame_dump_needs_flush = true;
}

bool FrameDumper::CheckFrameDumpRenderTexture(u32 target_width, u32 target_height)
{
  // Ensure framebuffer exists (we lazily allocate it in case frame dumping isn't used).
  // Or, resize texture if it isn't large enough to accommodate the current frame.
  if (m_frame_dump_render_texture && m_frame_dump_render_texture->GetWidth() == target_width &&
      m_frame_dump_render_texture->GetHeight() == target_height)
  {
    return true;
  }

  // Recreate texture, but release before creating so we don't temporarily use twice the RAM.
  m_frame_dump_render_framebuffer.reset();
  m_frame_dump_render_texture.reset();
  m_frame_dump_render_texture = g_gfx->CreateTexture(
      TextureConfig(target_width, target_height, 1, 1, 1, AbstractTextureFormat::RGBA8,
                    AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray),
      "Frame dump render texture");
  if (!m_frame_dump_render_texture)
  {
    PanicAlertFmt("Failed to allocate frame dump render texture");
    return false;
  }
  m_frame_dump_render_framebuffer =
      g_gfx->CreateFramebuffer(m_frame_dump_render_texture.get(), nullptr);
  ASSERT(m_frame_dump_render_framebuffer);
  return true;
}

bool FrameDumper::CheckFrameDumpReadbackTexture(u32 target_width, u32 target_height)
{
  std::unique_ptr<AbstractStagingTexture>& rbtex = m_frame_dump_readback_texture;
  if (rbtex && rbtex->GetWidth() == target_width && rbtex->GetHeight() == target_height)
    return true;

  rbtex.reset();
  rbtex = g_gfx->CreateStagingTexture(StagingTextureType::Readback,
                                      TextureConfig(target_width, target_height, 1, 1, 1,
                                                    AbstractTextureFormat::RGBA8, 0,
                                                    AbstractTextureType::Texture_2DArray));
  if (!rbtex)
    return false;

  return true;
}

void FrameDumper::FlushFrameDump()
{
  if (!m_frame_dump_needs_flush)
    return;

  // Ensure dumping thread is done with output texture before swapping.
  FinishFrameData();

  std::swap(m_frame_dump_output_texture, m_frame_dump_readback_texture);

  // Queue encoding of the last frame dumped.
  auto& output = m_frame_dump_output_texture;
  output->Flush();
  if (output->Map())
  {
    DumpFrameData(reinterpret_cast<u8*>(output->GetMappedPointer()), output->GetConfig().width,
                  output->GetConfig().height, static_cast<int>(output->GetMappedStride()));
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Failed to map texture for dumping.");
  }

  m_frame_dump_needs_flush = false;

  // Shutdown frame dumping if it is no longer active.
  if (!IsFrameDumping())
    ShutdownFrameDumping();
}

void FrameDumper::ShutdownFrameDumping()
{
  // Ensure the last queued readback has been sent to the encoder.
  FlushFrameDump();

  if (!m_frame_dump_thread_running.IsSet())
    return;

  // Ensure previous frame has been encoded.
  FinishFrameData();

  // Wake thread up, and wait for it to exit.
  m_frame_dump_thread_running.Clear();
  m_frame_dump_start.Set();
  Common::JoinThread(m_frame_dump_thread);
  m_frame_dump_render_framebuffer.reset();
  m_frame_dump_render_texture.reset();

  m_frame_dump_readback_texture.reset();
  m_frame_dump_output_texture.reset();
}

void FrameDumper::DumpFrameData(const u8* data, int w, int h, int stride)
{
  m_frame_dump_data = FrameData{data, w, h, stride};

  if (!m_frame_dump_thread_running.IsSet())
  {
    Common::JoinThread(m_frame_dump_thread);
    m_frame_dump_thread_running.Set();
    m_frame_dump_thread = sthread_create(Common::ThreadEntry<&FrameDumper::FrameDumpThreadFunc>, this);
  }

  // Wake worker thread up.
  m_frame_dump_start.Set();
  m_frame_dump_frame_running = true;
}

void FrameDumper::FinishFrameData()
{
  if (!m_frame_dump_frame_running)
    return;

  m_frame_dump_done.Wait();
  m_frame_dump_frame_running = false;

  m_frame_dump_output_texture->Unmap();
}

void FrameDumper::FrameDumpThreadFunc()
{
  Common::SetCurrentThreadName("FrameDumping");

  bool frame_dump_started = false;

  while (true)
  {
    m_frame_dump_start.Wait();
    if (!m_frame_dump_thread_running.IsSet())
      break;

    auto frame = m_frame_dump_data;

    // Save screenshot
    if (auto* name = static_cast<std::string*>(
            retro_atomic_exchange_ptr(&m_screenshot_request, nullptr)))
    {
      if (DumpFrameToPNG(frame, *name))
        Core::DisplayMessage("Screenshot saved to " + *name);

      delete name;
      m_screenshot_completed.Set();
    }

    if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    {
      if (!frame_dump_started)
      {
        frame_dump_started = StartFrameDumpToImage(frame);

        // Stop frame dumping if we fail to start.
        if (!frame_dump_started)
          Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, false);
      }

      // If we failed to start frame dumping, don't write a frame.
      if (frame_dump_started)
        DumpFrameToImage(frame);
    }

    m_frame_dump_done.Set();
  }
}

std::string FrameDumper::GetFrameDumpNextImageFileName() const
{
  return fmt::format("{}framedump_{}.png", File::GetUserPath(D_DUMPFRAMES_IDX),
                     m_frame_dump_image_counter);
}

bool FrameDumper::StartFrameDumpToImage(const FrameData&)
{
  m_frame_dump_image_counter = 1;
  if (!Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES_SILENT))
  {
    // Only check for the presence of the first image to confirm overwriting.
    // A previous run will always have at least one image, and it's safe to assume that if the user
    // has allowed the first image to be overwritten, this will apply any remaining images as well.
    std::string filename = GetFrameDumpNextImageFileName();
    if (File::Exists(filename))
    {
      if (!AskYesNoFmtT("Frame dump image(s) '{0}' already exists. Overwrite?", filename))
        return false;
    }
  }

  return true;
}

void FrameDumper::DumpFrameToImage(const FrameData& frame)
{
  DumpFrameToPNG(frame, GetFrameDumpNextImageFileName());
  m_frame_dump_image_counter++;
}

void FrameDumper::SaveScreenshot(std::string filename)
{
  delete static_cast<std::string*>(
      retro_atomic_exchange_ptr(&m_screenshot_request, new std::string(std::move(filename))));
}

bool FrameDumper::IsFrameDumping() const
{
  if (retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t*>(&m_screenshot_request)))
    return true;

  if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    return true;

  return false;
}

std::unique_ptr<FrameDumper> g_frame_dumper;
