// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "Common/GL/GLContext.h"

#include "DolphinLibretro/VideoContexts/GLContextLR.h"

const std::array<std::pair<int, int>, 9> GLContext::s_desktop_opengl_versions = {
    {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}}};

GLContext::~GLContext() = default;

bool GLContext::Initialize(bool stereo, bool core)
{
  return false;
}

bool GLContext::IsHeadless() const
{
  return true;
}

void GLContext::Update()
{
}

void GLContext::Swap()
{
}

void GLContext::SwapInterval(int interval)
{
}

void* GLContext::GetFuncAddress(const std::string& name)
{
  return nullptr;
}

std::unique_ptr<GLContext> GLContext::Create(bool stereo, bool core, bool prefer_gles)
{
  std::unique_ptr<GLContext> context = std::make_unique<GLContextLR>();

  // Option to prefer GLES on desktop platforms, useful for testing.
  if (prefer_gles)
    context->m_opengl_mode = Mode::OpenGLES;

  if (!context->Initialize(stereo, core))
    return nullptr;

  return context;
}
