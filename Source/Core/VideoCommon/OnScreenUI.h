// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"

class NativeVertexFormat;
class AbstractTexture;
class AbstractPipeline;
struct ImTextureData;

namespace VideoCommon
{
// OnScreenUI handles all the ImGui rendering.
class OnScreenUI
{
public:
  OnScreenUI() = default;
  ~OnScreenUI();

  // ImGui initialization depends on being able to create textures and pipelines, so do it last.
  bool Initialize(u32 width, u32 height, float scale);

  bool IsReady() { return m_ready; }

  // Everything here runs on the video thread, which alone touches the ImGui context.

  // Sets up ImGui state for the next frame.
  void BeginImGuiFrame(u32 width, u32 height);

  // Renders ImGui windows to the currently-bound framebuffer.
  void DrawImGui();

  // Recompiles ImGui pipeline - call when stereo mode changes.
  bool RecompileImGuiPipeline();

  void SetScale(float backbuffer_scale);

  void Finalize();

private:
  void DrawDebugText();
  void UpdateImguiTexture(ImTextureData* tex);

  // ImGui resources.
  std::unique_ptr<NativeVertexFormat> m_imgui_vertex_format;
  std::vector<std::unique_ptr<AbstractTexture>> m_imgui_textures;
  std::unique_ptr<AbstractPipeline> m_imgui_pipeline;
  u64 m_imgui_last_frame_time = 0;

  u32 m_backbuffer_width = 1;
  u32 m_backbuffer_height = 1;
  float m_backbuffer_scale = 1.0;


  bool m_ready = false;
};
}  // namespace VideoCommon
