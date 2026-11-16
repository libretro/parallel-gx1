// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLUtil.h"

#include "Common/GL/GLContext.h"
#include "Common/Logging/Log.h"

namespace GLUtil
{
void EnablePrimitiveRestart(const GLContext* context)
{
  constexpr GLuint PRIMITIVE_RESTART_INDEX = 65535;

  if (context->IsGLES())
  {
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
  }
  else
  {
    if (GLExtensions::Version() >= 310)
    {
      glEnable(GL_PRIMITIVE_RESTART);
      glPrimitiveRestartIndex(PRIMITIVE_RESTART_INDEX);
    }
    else
    {
      glEnableClientState(GL_PRIMITIVE_RESTART_NV);
      glPrimitiveRestartIndexNV(PRIMITIVE_RESTART_INDEX);
    }
  }
}
}  // namespace GLUtil
