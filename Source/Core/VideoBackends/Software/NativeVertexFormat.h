// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"

struct InputVertexData
{
  u8 posMtx;
  u8 texMtx[8];

  Common::Vec3 position;
  Common::Vec3 normal[3];
  u8 color[2][4];
  Common::Vec2 texCoords[8];
};

struct OutputVertexData
{
  // components in color channels
  enum
  {
    RED_C,
    GRN_C,
    BLU_C,
    ALP_C
  };

  Common::Vec3 mvPosition = {};
  Common::Vec4 projectedPosition = {};
  Common::Vec3 screenPosition = {};
  Common::Vec3 normal[3]{};
  u8 color[2][4]{};
  Common::Vec3 texCoords[8]{};

  void Lerp(float t, const OutputVertexData* a, const OutputVertexData* b)
  {
#define LINTERP(T, OUT, IN) (OUT) + ((IN - OUT) * T)

#define LINTERP_INT(T, OUT, IN) (OUT) + (((IN - OUT) * T) >> 8)

    mvPosition = LINTERP(t, a->mvPosition, b->mvPosition);

    projectedPosition.x = LINTERP(t, a->projectedPosition.x, b->projectedPosition.x);
    projectedPosition.y = LINTERP(t, a->projectedPosition.y, b->projectedPosition.y);
    projectedPosition.z = LINTERP(t, a->projectedPosition.z, b->projectedPosition.z);
    projectedPosition.w = LINTERP(t, a->projectedPosition.w, b->projectedPosition.w);

    for (std::size_t i = 0; i < 3; ++i)
    {
      normal[i] = LINTERP(t, a->normal[i], b->normal[i]);
    }

    const u16 t_int = static_cast<u16>(t * 256);
    for (std::size_t i = 0; i < 4; ++i)
    {
      color[0][i] = LINTERP_INT(t_int, a->color[0][i], b->color[0][i]);
      color[1][i] = LINTERP_INT(t_int, a->color[1][i], b->color[1][i]);
    }

    for (std::size_t i = 0; i < 8; ++i)
    {
      texCoords[i] = LINTERP(t, a->texCoords[i], b->texCoords[i]);
    }

#undef LINTERP
#undef LINTERP_INT
  }
};
