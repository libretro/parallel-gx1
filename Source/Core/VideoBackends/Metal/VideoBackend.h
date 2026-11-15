// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

namespace Metal
{
class VideoBackend final : public VideoBackendBase
{
public:
  VideoBackend() : VideoBackendBase(VIDEO_BACKEND_METAL) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const;
  std::string GetDisplayName() const;

  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "Metal";
};
}  // namespace Metal
