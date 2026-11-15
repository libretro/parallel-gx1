// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/VideoBackendBase.h"

namespace Null
{
class VideoBackend final : public VideoBackendBase
{
public:
  VideoBackend() : VideoBackendBase(VIDEO_BACKEND_NULL) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const { return CONFIG_NAME; }
  std::string GetDisplayName() const;
  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "Null";
};
}  // namespace Null
