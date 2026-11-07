// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Software/SWBoundingBox.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"

namespace BBoxManager
{
namespace
{
// Current bounding box coordinates.
std::array<u16, 4> s_coordinates{};
}  // Anonymous namespace

u16 GetCoordinate(Coordinate coordinate)
{
  return s_coordinates[static_cast<u32>(coordinate)];
}

void SetCoordinate(Coordinate coordinate, u16 value)
{
  s_coordinates[static_cast<u32>(coordinate)] = value;
}

void Update(u16 left, u16 right, u16 top, u16 bottom)
{
  const u16 new_left = std::min(left, GetCoordinate(Coordinate::Left));
  const u16 new_right = std::max(right, GetCoordinate(Coordinate::Right));
  const u16 new_top = std::min(top, GetCoordinate(Coordinate::Top));
  const u16 new_bottom = std::max(bottom, GetCoordinate(Coordinate::Bottom));

  SetCoordinate(Coordinate::Left, new_left);
  SetCoordinate(Coordinate::Right, new_right);
  SetCoordinate(Coordinate::Top, new_top);
  SetCoordinate(Coordinate::Bottom, new_bottom);
}

}  // namespace BBoxManager

namespace SW
{
void SWBoundingBox::Read(u32 index, u32 length, BBoxType* values)
{
  for (u32 i = 0; i < length; i++)
    values[i] = BBoxManager::GetCoordinate(static_cast<BBoxManager::Coordinate>(index + i));
}

void SWBoundingBox::Write(u32 index, const BBoxType* values, u32 length)
{
  for (u32 i = 0; i < length; i++)
  {
    BBoxManager::SetCoordinate(static_cast<BBoxManager::Coordinate>(index + i), values[i]);
  }
}

}  // namespace SW
