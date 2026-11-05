// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/MathUtil.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoCommon.h"

#include <array>
#include <memory>
#include <tuple>

class AbstractTexture;
struct SurfaceInfo;
enum class AspectMode : int;

namespace VideoCommon
{
class PostProcessing;
#ifdef __LIBRETRO__
extern bool g_is_duplicate_frame;
#endif
// Presenter is a class that deals with putting the final XFB on the screen.
// It also handles the ImGui UI and post-processing.
class Presenter
{
public:
  using ClearColor = std::array<float, 4>;

  Presenter();
  virtual ~Presenter();

  void ViSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);
  void ImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height);

  void SetNextSwapEstimatedTicks(u64 ticks);

  void Present();
  void ClearLastXfbId() { m_last_xfb_id = std::numeric_limits<u64>::max(); }

  bool Initialize();

  void ConfigChanged(u32 changed_bits);

  // Window resolution (display resolution if fullscreen)
  int GetBackbufferWidth() const { return m_backbuffer_width; }
  int GetBackbufferHeight() const { return m_backbuffer_height; }
  float GetBackbufferScale() const { return m_backbuffer_scale; }
  u32 AutoIntegralScale() const;
  AbstractTextureFormat GetBackbufferFormat() const { return m_backbuffer_format; }
  void SetSuggestedWindowSize(int width, int height);
  void SetBackbuffer(int backbuffer_width, int backbuffer_height);
  void SetBackbuffer(SurfaceInfo info);
  void OnBackbufferSet(bool size_changed, bool is_first_set);

  void UpdateDrawRectangle();

  // Get the amount of pixels the given rect should be cropped on all sides from custom cropping.
  MathUtil::Rectangle<int> GetCustomCrop(const MathUtil::Rectangle<int>& rect) const;

  // Crop the given rectangle by the custom cropping.
  MathUtil::Rectangle<int> AdjustForCustomCrop(const MathUtil::Rectangle<int>& rect) const;

  // Modify an aspect ratio by the aspect ratio change that custom cropping will apply.
  float AdjustAspectRatioForCustomCrop(float input_aspect_ratio) const;

  // Returns the target aspect ratio the XFB output should be drawn with.
  float CalculateDrawAspectRatio(bool allow_stretch = true) const;
  // The same for another aspect mode than the configured one
  float CalculateDrawAspectRatio(AspectMode aspect_mode, bool allow_stretch) const;

  // Crops the target rectangle to the framebuffer dimensions, reducing the size of the source
  // rectangle if it is greater. Works even if the source and target rectangles don't have a
  // 1:1 pixel mapping, scaling as appropriate.
  void AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                   MathUtil::Rectangle<int>* source_rect, int fb_width,
                                   int fb_height);

  void ReleaseXFBContentLock();

  // Draws the specified XFB buffer to the screen, performing any post-processing.
  // Assumes that the backbuffer has already been bound and cleared.
  virtual void RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                 const AbstractTexture* source_texture,
                                 const MathUtil::Rectangle<int>& source_rc);

  VideoCommon::PostProcessing* GetPostProcessor() const { return m_post_processor.get(); }

  int FrameCount() const { return m_frame_count; }

  void DoState(PointerWrap& p);

  const MathUtil::Rectangle<int>& GetTargetRectangle() const { return m_target_rectangle; }

private:
  // Fetches the XFB texture from the texture cache.
  // Returns true the contents have changed since last time
  bool FetchXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);

  void ProcessFrameDumping(u64 ticks) const;

  void OnBackBufferSizeChanged();

  // Scales a raw XFB resolution to the target (display) aspect ratio,
  // also accounting for crop and other minor adjustments
  std::tuple<int, int> CalculateOutputDimensions(int width, int height,
                                                 bool allow_stretch = true) const;
  std::tuple<float, float> ApplyStandardAspectCrop(float width, float height,
                                                   bool allow_stretch = true) const;
  // Scales a raw XFB resolution to the target (display) aspect ratio
  std::tuple<float, float> ScaleToDisplayAspectRatio(int width, int height,
                                                     bool allow_stretch = true) const;

  // Use this to convert a single target rectangle to two stereo rectangles
  std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
  ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const;


  // Backbuffer (window) size and render area
  int m_backbuffer_width = 0;
  int m_backbuffer_height = 0;
  float m_backbuffer_scale = 1.0f;
  AbstractTextureFormat m_backbuffer_format = AbstractTextureFormat::Undefined;


  // The presentation rectangle.
  // Width and height correspond to the final output resolution.
  // Offsets imply black borders (if the window aspect ratio doesn't match the game's one).
  MathUtil::Rectangle<int> m_target_rectangle = {};

  u32 m_auto_resolution_scale = 1;

  RcTcacheEntry m_xfb_entry;
  // Internal resolution multiplier scaled XFB size
  MathUtil::Rectangle<int> m_xfb_rect{0, 0, MAX_XFB_WIDTH, MAX_XFB_HEIGHT};

  // Tracking of XFB textures so we don't render duplicate frames.
  u64 m_last_xfb_id = std::numeric_limits<u64>::max();

  // These will be set on the first call to SetSuggestedWindowSize.
  int m_last_window_request_width = 0;
  int m_last_window_request_height = 0;

  std::unique_ptr<VideoCommon::PostProcessing> m_post_processor;

  u64 m_frame_count = 0;
  u64 m_present_count = 0;

  // XFB tracking
  u64 m_last_xfb_ticks = 0;
  u32 m_last_xfb_addr = 0;
  // Native XFB width
  u32 m_last_xfb_width = MAX_XFB_WIDTH;
  u32 m_last_xfb_stride = 0;
  // Native XFB height
  u32 m_last_xfb_height = MAX_XFB_HEIGHT;

  Common::EventHook m_config_changed;
  Common::EventHook m_end_field_hook;

  // Calculated from the previous swap time and current refresh rate.
  // Can be used for presentation of ImmediateXFB swaps which don't have timing information.
  u64 m_next_swap_estimated_ticks = 0;

  std::atomic_bool m_immediate_swap_happened_this_field{};
};

}  // namespace VideoCommon

extern std::unique_ptr<VideoCommon::Presenter> g_presenter;
