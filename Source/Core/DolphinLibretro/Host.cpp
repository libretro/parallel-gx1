
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Host.h"

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

bool Host_TASInputHasFocus() { return false; }

void Host_Message(HostMessageID id)
{
  DEBUG_LOG(COMMON, "message id: %d\n", (int)id);
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  /* called on input poll */
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

void Host_TitleChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}
