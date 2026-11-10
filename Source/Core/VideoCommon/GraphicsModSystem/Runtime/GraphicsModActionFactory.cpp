// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/MoveAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/PrintAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/ScaleAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/SkipAction.h"

namespace GraphicsModActionFactory
{
std::unique_ptr<GraphicsModAction> Create(std::string_view name, const Common::Vec3& xyz,
                                          std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  if (name == PrintAction::factory_name)
  {
    return std::make_unique<PrintAction>();
  }
  else if (name == SkipAction::factory_name)
  {
    return std::make_unique<SkipAction>();
  }
  else if (name == MoveAction::factory_name)
  {
    return std::make_unique<MoveAction>(xyz);
  }
  else if (name == ScaleAction::factory_name)
  {
    return std::make_unique<ScaleAction>(xyz);
  }

  return nullptr;
}
}  // namespace GraphicsModActionFactory
