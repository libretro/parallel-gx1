// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/MoveAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/PrintAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/ScaleAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/SkipAction.h"

namespace GraphicsModActionFactory
{
GraphicsModAction* Create(std::string_view name, const Common::Vec3& xyz)
{
  if (name == PrintAction::factory_name)
  {
    return new PrintAction();
  }
  else if (name == SkipAction::factory_name)
  {
    return new SkipAction();
  }
  else if (name == MoveAction::factory_name)
  {
    return new MoveAction(xyz);
  }
  else if (name == ScaleAction::factory_name)
  {
    return new ScaleAction(xyz);
  }

  return nullptr;
}
}  // namespace GraphicsModActionFactory
