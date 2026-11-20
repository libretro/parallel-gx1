// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// The frontend's OpenGL context, through libretro's hardware rendering interface
class GLContext
{
public:
  enum class Mode
  {
    Detect,
    OpenGL,
    OpenGLES
  };

  Mode GetMode() const { return m_opengl_mode; }
  bool IsGLES() const { return m_opengl_mode == Mode::OpenGLES; }

  u32 GetBackBufferWidth() const { return m_backbuffer_width; }
  u32 GetBackBufferHeight() const { return m_backbuffer_height; }

  void Swap();

  void* GetFuncAddress(const char* name);

  // Takes the frontend's context: false until it is ready.
  bool Initialize();

private:
  Mode m_opengl_mode = Mode::Detect;

  // Window dimensions.
  u32 m_backbuffer_width = 0;
  u32 m_backbuffer_height = 0;
};
