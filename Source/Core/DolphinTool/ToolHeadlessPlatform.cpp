// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <string>

#include "Core/Host.h"

// Begin stubs needed to satisfy Core dependencies

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(HostMessageID id)
{
}

void Host_UpdateTitle(const std::string& title)
{
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return false;
}

bool Host_RendererHasFullFocus()
{
  return false;
}

bool Host_RendererIsFullscreen()
{
  return false;
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}
// End stubs to satisfy Core dependencies
