// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

class GLContext;

namespace OGL
{
class VideoBackend final : public VideoBackendBase
{
public:
  VideoBackend() : VideoBackendBase(VIDEO_BACKEND_OGL) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const;
  std::string GetDisplayName() const;

  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "OGL";

public:
  bool InitializeGLExtensions(GLContext* context);
  bool FillBackendInfo(GLContext* context);
};
}  // namespace OGL
