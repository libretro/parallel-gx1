// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

namespace SW
{
class VideoSoftware : public VideoBackendBase
{
  bool Initialize(float backbuffer_scale) override;
  void Shutdown() override;

  std::string GetConfigName() const override;
  std::string GetDisplayName() const override;

  void InitBackendInfo() override;

  static constexpr const char* CONFIG_NAME = "Software Renderer";
};
}  // namespace SW
