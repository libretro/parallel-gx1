// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

class SkipAction final : public GraphicsModAction
{
public:
  static constexpr char factory_name[] = "skip";
  void OnDrawStarted(GraphicsModActionData::DrawStarted*) override;
  void BeforeEFB(GraphicsModActionData::PreEFB*) override;
};
