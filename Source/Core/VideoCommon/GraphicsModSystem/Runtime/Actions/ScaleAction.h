// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string_view>

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

class ScaleAction final : public GraphicsModAction
{
public:
  static constexpr std::string_view factory_name = "scale";
  explicit ScaleAction(Common::Vec3 scale);
  void BeforeEFB(GraphicsModActionData::PreEFB*) override;
  void OnProjection(GraphicsModActionData::Projection*) override;
  void OnProjectionAndTexture(GraphicsModActionData::Projection*) override;

private:
  Common::Vec3 m_scale;
};
