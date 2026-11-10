// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/MoveAction.h"

MoveAction::MoveAction(Common::Vec3 position_offset) : m_position_offset(position_offset)
{
}

void MoveAction::OnProjection(GraphicsModActionData::Projection* projection)
{
  if (!projection) [[unlikely]]
    return;

  if (!projection->matrix) [[unlikely]]
    return;

  auto& matrix = *projection->matrix;
  matrix = matrix * Common::Matrix44::Translate(m_position_offset);
}

void MoveAction::OnProjectionAndTexture(GraphicsModActionData::Projection* projection)
{
  if (!projection) [[unlikely]]
    return;

  if (!projection->matrix) [[unlikely]]
    return;

  auto& matrix = *projection->matrix;
  matrix = matrix * Common::Matrix44::Translate(m_position_offset);
}
