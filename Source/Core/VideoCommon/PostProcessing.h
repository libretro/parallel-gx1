// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureConfig.h"

class AbstractPipeline;
class AbstractShader;
class AbstractTexture;

namespace VideoCommon
{
// The final pass from the XFB to the output: a plain copy, or, when the output is resampled,
// colour corrected or HDR, the built-in correction shader.
class PostProcessing
{
public:
  PostProcessing();
  ~PostProcessing();

  bool Initialize(AbstractTextureFormat format);
  void RecompilePipeline();

  void BlitFromTexture(const MathUtil::Rectangle<int>& dst, const MathUtil::Rectangle<int>& src,
                       const AbstractTexture* src_tex, int src_layer = -1);
  /* src_tex's src, point sampled, into dst's colour; its alpha is left */
  void BlitScreen(const MathUtil::Rectangle<int>& dst, const MathUtil::Rectangle<int>& src,
                  const AbstractTexture* src_tex);

private:
  bool IsColorCorrectionActive() const;
  bool CompileShaders();
  void CompileCorrectionShader();
  bool CompilePipelines();
  void DestroyPipelines();
  void FillUniformBuffer(const MathUtil::Rectangle<int>& src, const AbstractTexture* src_tex,
                         int src_layer, const MathUtil::Rectangle<int>& dst,
                         const MathUtil::Rectangle<int>& wnd);

  AbstractShader* m_vertex_shader = nullptr;
  AbstractShader* m_copy_pixel_shader = nullptr;
  AbstractShader* m_correction_pixel_shader = nullptr;
  bool m_correction_shader_tried = false;
  AbstractPipeline* m_copy_pipeline = nullptr;
  AbstractPipeline* m_correction_pipeline = nullptr;
  AbstractPipeline* m_screen_pipeline = nullptr;
  std::vector<u8> m_uniform_staging_buffer;

  AbstractTextureFormat m_framebuffer_format = AbstractTextureFormat::Undefined;
};
}  // namespace VideoCommon
