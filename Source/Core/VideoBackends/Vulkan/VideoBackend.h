// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Common.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Vulkan
{
class VideoBackend final : public VideoBackendBase
{
public:
  VideoBackend() : VideoBackendBase(VIDEO_BACKEND_VULKAN) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const { return CONFIG_NAME; }
  std::string GetDisplayName() const { return _trans("Vulkan"); }
  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "Vulkan";
};
}  // namespace Vulkan
