// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/AbstractDeleters.h"

#include "Common/HookableEvent.h"
#include "Common/MathUtil.h"

#include "VideoCommon/RenderState.h"

#include <array>
#include <memory>
#include <vector>

class AbstractFramebuffer;
class AbstractPipeline;
class AbstractShader;
class AbstractTexture;
class AbstractStagingTexture;
class NativeVertexFormat;
struct ComputePipelineConfig;
struct AbstractPipelineConfig;
struct PortableVertexDeclaration;
struct TextureConfig;
enum class AbstractTextureFormat : u32;
enum class ShaderStage;
enum class StagingTextureType;

struct SurfaceInfo
{
  u32 width = 0;
  u32 height = 0;
  float scale = 0.0f;
  AbstractTextureFormat format = {};
};

using ClearColor = std::array<float, 4>;

// AbstractGfx is the root of Dolphin's Graphics API abstraction layer.
//
// Abstract knows nothing about the internals of the GameCube/Wii, that is all handled elsewhere in
// VideoCommon.

class ShaderCode;

class AbstractGfx
{
public:
  AbstractGfx();

  bool IsHeadless() const;

  // Does the backend support drawing a UI or doing post-processing
  bool SupportsUtilityDrawing() const;

  void SetPipeline(const AbstractPipeline* pipeline);
  void SetScissorRect(const MathUtil::Rectangle<int>& rc);
  void SetTexture(u32 index, const AbstractTexture* texture);
  void SetSamplerState(u32 index, const SamplerState& state);
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write);
  void UnbindTexture(const AbstractTexture* texture);
  void SetViewport(float x, float y, float width, float height, float near_depth, float far_depth);
  void BeginUtilityDrawing();
  void EndUtilityDrawing();
  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name = "");
  std::unique_ptr<AbstractStagingTexture> CreateStagingTexture(StagingTextureType type,
                                                               const TextureConfig& config);
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments = {});

  // Framebuffer operations.
  void SetFramebuffer(AbstractFramebuffer* framebuffer);
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer);
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f);

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z);

  // Drawing with currently-bound pipeline state.
  void Draw(u32 base_vertex, u32 num_vertices);
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex);

  // Dispatching compute shaders with currently-bound state.
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z);

  // Binds the backbuffer for rendering. The buffer will be cleared immediately after binding.
  // This is where any window size changes are detected, therefore m_backbuffer_width and/or
  // m_backbuffer_height may change after this function returns.
  // If this returns false, a problem occurred binding the backbuffer.
  // Don't render anything to it, but still call `PresentBackbuffer`, which will reset any
  // per-frame resources and prepare for the next frame.
  bool BindBackbuffer(const ClearColor& clear_color = {});

  // Presents the backbuffer to the window system, or "swaps buffers".
  void PresentBackbuffer();

  /* Presents the top-left width x height of a texture as the picture, in
   * place of BindBackbuffer, a draw and PresentBackbuffer; false when the
   * backend cannot, and nothing was done. */
  bool PresentTexture(const AbstractTexture* texture, u32 width, u32 height);

  /* Whether a texture given to PresentTexture may still be read by whoever
   * shows the pictures; kept alive by the caller until it is not. */
  bool IsTextureHeld(const AbstractTexture* texture);

  // Shader modules/objects. The source's text is the backend's to write its header in front
  // of, or to take.
  AbstractShader* CreateShaderFromSource(ShaderStage stage, ShaderCode* source,
                                                         std::string_view name = "");
  AbstractShader* CreateShaderFromSource(ShaderStage stage, ShaderCode& source,
                                                         std::string_view name = "")
  {
    return CreateShaderFromSource(stage, &source, name);
  }
  AbstractShader* CreateShaderFromSource(ShaderStage stage, ShaderCode&& source,
                                                         std::string_view name = "")
  {
    return CreateShaderFromSource(stage, &source, name);
  }
  AbstractShader* CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length, std::string_view name = "");
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl);
  AbstractPipeline* CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0);

  AbstractFramebuffer* GetCurrentFramebuffer() const { return m_current_framebuffer; }

  struct ViewportAndScissor
  {
    MathUtil::Rectangle<int> scissor_rect;
    float viewport_x = 0;
    float viewport_y = 0;
    float viewport_width = 0;
    float viewport_height = 0;
    float viewport_near_depth = 0;
    float viewport_far_depth = 0;
  };

  // Stores the last viewport and scissor, the stored data is restored in 'EndUtilityDrawing'
  void StoreViewportAndScissor(const ViewportAndScissor& viewport_and_scissor);

  // Sets viewport and scissor to the specified rectangle. rect is assumed to be in framebuffer
  // coordinates, i.e. lower-left origin in OpenGL.
  void SetViewportAndScissor(const MathUtil::Rectangle<int>& rect, float min_depth = 0.0f,
                             float max_depth = 1.0f);

  // Scales a GPU texture using a copy shader.
  void ScaleTexture(AbstractFramebuffer* dst_framebuffer, const MathUtil::Rectangle<int>& dst_rect,
                    const AbstractTexture* src_texture, const MathUtil::Rectangle<int>& src_rect);

  // Converts an upper-left to lower-left if required by the backend, optionally
  // clamping to the framebuffer size.
  MathUtil::Rectangle<int> ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                                                       u32 fb_width, u32 fb_height) const;
  MathUtil::Rectangle<int>
  ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                              const AbstractFramebuffer* framebuffer) const;

  void Flush();
  void WaitForGPUIdle();

  // For opengl's glDrawBuffer
  void SelectLeftBuffer();
  void SelectRightBuffer();
  void SelectMainBuffer();

  // A simple presentation fallback, only used by video software
  void ShowImage(const AbstractTexture* source_texture, const MathUtil::Rectangle<int>& source_rc);

  // Whether the driver compiles on threads of its own (AsyncShaderCompiler::SetDeferred).
  bool CompilesShadersOnItsOwnThreads() const;

  // Called when the configuration changes, and backend structures need to be updated.
  void OnConfigChanged(u32 changed_bits);

  // Returns true if a layer-expanding geometry shader should be used when rendering
  // the user interface on the output buffer.
  bool UseGeometryShaderForUI() const;

  // Returns info about the main surface (aka backbuffer)
  SurfaceInfo GetSurfaceInfo() const;

protected:
  // What a backend's Gfx does not do itself; it may also call these from its own.
  void BaseBeginUtilityDrawing();
  void BaseEndUtilityDrawing();
  void BaseSetFramebuffer(AbstractFramebuffer* framebuffer);
  void BaseSetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer);
  void BaseSetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                  float depth_value);
  void BaseClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                       bool alphaEnable, bool zEnable, u32 color, u32 z);
  void BaseScaleTexture(AbstractFramebuffer* dst_framebuffer,
                        const MathUtil::Rectangle<int>& dst_rect,
                        const AbstractTexture* src_texture,
                        const MathUtil::Rectangle<int>& src_rect);
  void BaseOnConfigChanged(u32 changed_bits);

  AbstractFramebuffer* m_current_framebuffer = nullptr;
  const AbstractPipeline* m_current_pipeline = nullptr;

private:
  Common::EventHook m_config_changed;
  ViewportAndScissor m_viewport_and_scissor;
};

extern std::unique_ptr<AbstractGfx> g_gfx;
