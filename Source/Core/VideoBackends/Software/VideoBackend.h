// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

namespace SW
{
class VideoSoftware final : public VideoBackendBase
{
public:
  VideoSoftware() : VideoBackendBase(VIDEO_BACKEND_SOFTWARE) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const;
  std::string GetDisplayName() const;

  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "Software Renderer";
};
}  // namespace SW
