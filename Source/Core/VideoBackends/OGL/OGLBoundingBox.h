// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/BoundingBox.h"

namespace OGL
{
class OGLBoundingBox final : public BoundingBox
{
  /* called by the base class as this backend */
  friend class ::BoundingBox;

public:
  ~OGLBoundingBox();

  bool Initialize();

protected:
  void Read(u32 index, u32 length, BBoxType* values);
  void Write(u32 index, const BBoxType* values, u32 length);

private:
  GLuint m_buffer_id = 0;
};

}  // namespace OGL
