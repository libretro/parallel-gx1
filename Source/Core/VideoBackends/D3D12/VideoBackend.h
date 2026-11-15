// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "VideoCommon/VideoBackendBase.h"

namespace DX12
{
class VideoBackend final : public VideoBackendBase
{
public:
  VideoBackend() : VideoBackendBase(VIDEO_BACKEND_D3D12) {}

  bool Initialize(float backbuffer_scale);
  void Shutdown();

  std::string GetConfigName() const;
  std::string GetDisplayName() const;
  void InitBackendInfo();

  static constexpr const char* CONFIG_NAME = "D3D12";

  // Again, from the frontend's device once the context has it.
  void FillD3D12BackendInfo() { FillBackendInfo(); }

private:
  void FillBackendInfo();
};
}  // namespace DX12
