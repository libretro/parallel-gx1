// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoBackendKind.h"

namespace MMIO
{
class Mapping;
}
class PointerWrap;

class AbstractGfx;
class BoundingBox;
class TextureCacheBase;
class VertexManagerBase;

enum class FieldType
{
  Odd,
  Even,
};

class VideoBackendBase
{
public:
  // These branch to the backend this is
  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const;
  std::string GetDisplayName() const;
  void InitBackendInfo();

  static std::string BadShaderFilename(const char* shader_stage, int counter);

  void Video_OutputXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);

  u32 Video_GetQueryResult(PerfQueryType type);
  u16 Video_GetBoundingBox(int index);

  static std::string GetDefaultBackendConfigName();
  static std::string GetDefaultBackendDisplayName();
  static void ActivateBackend(const std::string& name);

  // Fills the backend_info fields with the capabilities of the selected backend/device.
  static void PopulateBackendInfo();

  // Wrapper function which pushes the event to the GPU thread.
  void DoState(PointerWrap& p);

  // Every backend, with its own gfx, vertex manager, perf query and bounding box
  bool InitializeShared(AbstractGfx* gfx,
                        VertexManagerBase* vertex_manager,
                        PerfQueryBase* perf_query,
                        BoundingBox* bounding_box);
  void ShutdownShared();

  const VideoBackendKind backend_kind;

protected:
  explicit VideoBackendBase(VideoBackendKind kind) : backend_kind(kind) {}
  ~VideoBackendBase() = default;

  bool m_initialized = false;
};

extern VideoBackendBase* g_video_backend;
