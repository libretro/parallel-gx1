// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "Common/GL/GLContext.h"

#include <libretro.h>

#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"

const std::array<std::pair<int, int>, 9> GLContext::s_desktop_opengl_versions = {
    {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}}};

GLContext::~GLContext() = default;

bool GLContext::Initialize(bool stereo, bool core)
{
  const int efbScale =
      Libretro::Options::GetCached<int>(Libretro::Options::gfx_settings::EFB_SCALE, 1);
  m_backbuffer_width = Libretro::Video::GetAdjustedBaseWidth() * efbScale;
  m_backbuffer_height = Libretro::Video::GetAdjustedBaseHeight() * efbScale;

  switch (Libretro::Video::hw_render.context_type)
  {
  case RETRO_HW_CONTEXT_OPENGLES_VERSION:
  case RETRO_HW_CONTEXT_OPENGLES3:
    m_opengl_mode = Mode::OpenGLES;
    break;
  case RETRO_HW_CONTEXT_OPENGL_CORE:
  case RETRO_HW_CONTEXT_OPENGL:
  default:
    m_opengl_mode = Mode::OpenGL;
    break;
  }

  return g_context_status.IsReady();
}

void GLContext::Swap()
{
  Libretro::Video::video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_backbuffer_width, m_backbuffer_height,
                            0);
}

void* GLContext::GetFuncAddress(const std::string& name)
{
  return (void*)Libretro::Video::hw_render.get_proc_address(name.c_str());
}

std::unique_ptr<GLContext> GLContext::Create(bool stereo, bool core, bool prefer_gles)
{
  std::unique_ptr<GLContext> context = std::make_unique<GLContext>();

  // Option to prefer GLES on desktop platforms, useful for testing.
  if (prefer_gles)
    context->m_opengl_mode = Mode::OpenGLES;

  if (!context->Initialize(stereo, core))
    return nullptr;

  return context;
}
