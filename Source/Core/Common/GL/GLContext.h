// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"

class GLContext
{
public:
  enum class Mode
  {
    Detect,
    OpenGL,
    OpenGLES
  };

  virtual ~GLContext();

  Mode GetMode() const { return m_opengl_mode; }
  bool IsGLES() const { return m_opengl_mode == Mode::OpenGLES; }

  u32 GetBackBufferWidth() const { return m_backbuffer_width; }
  u32 GetBackBufferHeight() const { return m_backbuffer_height; }

  virtual bool IsHeadless() const;

  virtual void Update();

  virtual void Swap();
  virtual void SwapInterval(int interval);

  virtual void* GetFuncAddress(const std::string& name);

  // Creates an instance of GLContext specific to the platform we are running on.
  // If successful, the context is made current on the calling thread.
  static std::unique_ptr<GLContext> Create(bool stereo = false, bool core = true,
                                           bool prefer_gles = false);

protected:
  virtual bool Initialize(bool stereo, bool core);

  Mode m_opengl_mode = Mode::Detect;

  // Window dimensions.
  u32 m_backbuffer_width = 0;
  u32 m_backbuffer_height = 0;

  // A list of desktop OpenGL versions to attempt to create a context for.
  // (4.6-3.2, geometry shaders is a minimum requirement since we're using core profile).
  static const std::array<std::pair<int, int>, 9> s_desktop_opengl_versions;
};
